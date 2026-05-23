#pragma once

#include <cassert>
#include <concepts>
#include <memory_resource>
#include <new>
#include <type_traits>
#include <utility>

#include <actor-zeta/config.hpp>
#include <actor-zeta/detail/coro_frame_header.hpp>
#include <actor-zeta/detail/future_awaiters.hpp>
#include <actor-zeta/detail/shared_state.hpp>
#include <actor-zeta/detail/type_traits.hpp>

namespace actor_zeta {

    // Forward declarations for generator support
    template<typename T> class generator;
    namespace detail {
        template<typename T> class generator_state;
        template<typename T> struct next_awaiter;
    }

    namespace detail {
        template<typename T>
        concept has_resource_method = requires(T* ptr) {
            { ptr->resource() } -> std::convertible_to<std::pmr::memory_resource*>;
        };
    } // namespace detail

    template<typename T>
    class unique_future;

    // This is the user-facing promise for explicit promise/future pairs
    template<typename T>
    class promise final {
    private:
        static constexpr bool is_void_type = std::is_void_v<T>;
        using state_type = detail::shared_state<T>;

    public:
        promise() = delete;
        promise(const promise&) = delete;
        promise& operator=(const promise&) = delete;

        // Constructor - creates new shared_state (owning)
        explicit promise(std::pmr::memory_resource* res)
            : state_(detail::allocate_shared_state<T>(res)) {
            assert(res && "promise constructed with null resource");
        }

        // Constructor from existing shared_state (view, for message.get_result_promise)
        explicit promise(state_type* state) noexcept
            : state_(state) {}

        // Move constructor
        promise(promise&& other) noexcept
            : state_(std::exchange(other.state_, nullptr)) {}

        // Move assignment
        promise& operator=(promise&& other) noexcept {
            if (this != &other) {
                release_if_needed();
                state_ = std::exchange(other.state_, nullptr);
            }
            return *this;
        }

        // Destructor - sets broken_pipe if not set_value'd
        ~promise() noexcept {
            release_if_needed();
        }

        // Get future for this promise
        [[nodiscard]] unique_future<T> get_future() noexcept;

        // Set value (non-void)
        template<typename U>
            requires(!std::is_void_v<T> && std::is_constructible_v<T, U&&>)
        void set_value(U&& value) noexcept {
            assert(state_ && "set_value() on moved-from promise");
            state_->set_value(T(std::forward<U>(value)));
            // Thread safety (Q6): Do NOT take/resume continuation here.
            // Consumer will resume in its own actor's resume_impl().
            // Set finalizing flag before release to prevent race with release_future
            state_->flags_.fetch_or(detail::state_flags::promise_finalizing, std::memory_order_release);
            // Atomically release promise and check if cancelled.
            bool cancelled = state_->release_promise();
            if (cancelled) {
                state_ = nullptr;
                return;  // Future was already released
            }
            // Try to complete finalize phase (clears finalizing flag atomically)
            // Returns false if future was released concurrently (already deallocated)
            if (!state_->try_complete_finalize()) {
                state_ = nullptr;
                return;  // Cancelled after release_promise, state deallocated
            }
            state_ = nullptr;  // ownership transferred
            // NO cont.resume() - consumer resumes in its own thread
        }

        // Set value (void)
        void set_value() noexcept
            requires(std::is_void_v<T>)
        {
            assert(state_ && "set_value() on moved-from promise");
            state_->set_value();
            // Thread safety (Q6): Do NOT take/resume continuation here.
            // Consumer will resume in its own actor's resume_impl().
            // Set finalizing flag before release to prevent race with release_future
            state_->flags_.fetch_or(detail::state_flags::promise_finalizing, std::memory_order_release);
            bool cancelled = state_->release_promise();
            if (cancelled) {
                state_ = nullptr;
                return;
            }
            // Try to complete finalize phase
            if (!state_->try_complete_finalize()) {
                state_ = nullptr;
                return;
            }
            state_ = nullptr;
            // NO cont.resume() - consumer resumes in its own thread
        }

        // Set error (also the cancellation channel:
        // p.error(std::make_error_code(std::errc::operation_canceled)))
        void error(std::error_code ec) noexcept {
            assert(state_ && "error() on moved-from promise");
            state_->set_error(ec);
            // Thread safety (Q6): Do NOT take/resume continuation here.
            // Consumer will resume in its own actor's resume_impl().
            // Set finalizing flag before release to prevent race with release_future
            state_->flags_.fetch_or(detail::state_flags::promise_finalizing,
                                    std::memory_order_release);
            bool cancelled = state_->release_promise();
            if (cancelled) {
                state_ = nullptr;
                return;
            }
            // Try to complete finalize phase
            if (!state_->try_complete_finalize()) {
                state_ = nullptr;
                return;
            }
            state_ = nullptr;
            // NO cont.resume() - consumer resumes in its own thread
        }

        [[nodiscard]] bool valid() const noexcept {
            return state_ != nullptr;
        }

        [[nodiscard]] state_type* internal_state() const noexcept {
            return state_;
        }

    private:
        void release_if_needed() noexcept {
            if (state_) {
                state_->set_error(std::make_error_code(std::errc::broken_pipe));
                // Thread safety (Q6): Do NOT take/resume continuation here.
                // Consumer will resume in its own actor's resume_impl().
                // Set finalizing flag before release to prevent race with release_future
                state_->flags_.fetch_or(detail::state_flags::promise_finalizing, std::memory_order_release);
                bool cancelled = state_->release_promise();
                if (cancelled) {
                    state_ = nullptr;
                    return;
                }
                // Try to complete finalize phase
                if (!state_->try_complete_finalize()) {
                    state_ = nullptr;
                    return;
                }
                state_ = nullptr;
                // NO cont.resume() - consumer resumes in its own thread
            }
        }

        state_type* state_;
    };

    template<typename T>
    class unique_future final {
    public:
        // Forward declaration for promise_type (must be public for coroutine)
        struct promise_type;

    private:
        static constexpr bool is_void_type = std::is_void_v<T>;
        using state_type = detail::shared_state<T>;

    public:
        unique_future(const unique_future&) = delete;
        unique_future& operator=(const unique_future&) = delete;

        // Default constructor
        unique_future() noexcept
            : state_(nullptr)
            , handle_{} {}

        // Constructor for caller's future (from promise.get_future())
        explicit unique_future(state_type* s) noexcept
            : state_(s)
            , handle_{} {}

        // Constructor for method's future (from promise_type.get_return_object())
        unique_future(detail::coroutine_handle<promise_type> h, state_type* s) noexcept
            : state_(s)
            , handle_(h) {}

        // Move constructor
        unique_future(unique_future&& other) noexcept
            : state_(std::exchange(other.state_, nullptr))
            , handle_(std::exchange(other.handle_, {})) {}

        // Move assignment
        unique_future& operator=(unique_future&& other) noexcept {
            if (this != &other) {
                release();
                state_ = std::exchange(other.state_, nullptr);
                handle_ = std::exchange(other.handle_, {});
            }
            return *this;
        }

        // Destructor - Last-One-Out
        ~unique_future() noexcept {
            release();
        }

        // === MAIN API: co_await ===

        auto operator co_await() noexcept {
            return awaiter{state_};
        }

        // === Non-blocking value extraction (post-pump) ===

        // take_ready() - extract the value of an already-ready future without blocking.
        // Asserts readiness instead of waiting. Replaces the removed blocking get().
        [[nodiscard]] T take_ready() && requires(!std::is_void_v<T>) {
            assert(state_ && state_->has_result() && !state_->has_error()
                   && "take_ready() on a future that is not ready or completed with error");
            T r = state_->take_value();
            release();
            return r;
        }

        void take_ready() && requires(std::is_void_v<T>) {
            assert(state_ && state_->has_result() && !state_->has_error()
                   && "take_ready() on a future that is not ready or completed with error");
            release();
        }

        // is_ready for polling - checks promise_released
        [[nodiscard]] bool is_ready() const noexcept {
            return state_ && state_->is_ready();
        }

        [[nodiscard]] bool valid() const noexcept {
            return state_ != nullptr;
        }

        // failed()/error() are the cancellation-observation channel:
        // cancellation is produced via promise<T>::error(operation_canceled).
        [[nodiscard]] bool failed() const noexcept {
            return state_ && state_->has_error();
        }

        [[nodiscard]] std::error_code error() const noexcept {
            return state_ ? state_->get_error() : std::error_code{};
        }

        // Detach (fire-and-forget)
        void detach() noexcept {
            release();
        }

        // Internal access (for message class)
        [[nodiscard]] state_type* internal_state() const noexcept {
            return state_;
        }

        // Access to coroutine handle (for propagating awaited state)
        [[nodiscard]] detail::coroutine_handle<promise_type> coroutine_handle() const noexcept {
            return handle_;
        }

        // === Awaiter (Variant B+ with CAS) ===
        // Public so await_transform can access it from other unique_future<U> instantiations
        struct awaiter {
            state_type* state_;

            bool await_ready() const noexcept {
                // Fast path: if result already exists - don't suspend
                return state_->has_result();
            }

            detail::coroutine_handle<> await_suspend(detail::coroutine_handle<> h) noexcept {
                // CAS for setting continuation
                // This allows detecting double-await (programmer error)

                detail::coroutine_handle<> expected = nullptr;
                if (state_->continuation_.compare_exchange_strong(
                        expected, h,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {

                    // CAS successful - we set continuation
                    // Now check: maybe result is already ready?
                    if (state_->flags_.load(std::memory_order_acquire)
                            & detail::state_flags::result_set) {
                        // Result is ready! Try to take continuation back
                        auto cont = state_->continuation_.exchange(
                            nullptr, std::memory_order_acquire);
                        if (cont) {
                            // We took it - resume ourselves
                            return cont;
                        }
                        // Producer already took it - they will resume us
                        return detail::noop_coroutine();
                    }

                    // Result not ready - wait, producer will resume us
                    return detail::noop_coroutine();

                } else {
                    // CAS failed - someone already set continuation
                    // For single-consumer this is a programmer error
                    assert(false && "double co_await on unique_future is undefined behavior");

                    // In release: result should be ready (producer took old cont)
                    return h;  // resume ourselves
                }
            }

            auto await_resume() {
                // Check error before returning value
                assert(!state_->has_error() && "future completed with error");
                if constexpr (std::is_void_v<T>) {
                    state_->take_value();
                } else {
                    return state_->take_value();
                }
            }
        };

    private:
        void release() noexcept {
            if (state_) {
                state_->release_future();
                state_ = nullptr;
            }
            handle_ = {};
            // DO NOT call handle_.destroy() - coroutine destroys itself in final_suspend
        }

        // CRTP base: PromiseDerived is the final promise type.
        // Inherits the shared awaiter machinery (await_transform overloads + lock-free CAS +
        // awaited-chain propagation) from detail::future_awaiter_mixin (future_awaiters.hpp).
        template<typename PromiseDerived>
        struct promise_type_base : detail::future_awaiter_mixin<PromiseDerived> {
            using value_type = T;

            std::pmr::memory_resource* resource_ = nullptr;
            state_type* state_ = nullptr;

            // Creates OWN state, returns future with handle + state
            unique_future<T> get_return_object() {
                assert(resource_ != nullptr &&
                       "Coroutine must be actor member function with resource() method");
                if (!resource_) {
                    std::abort();
                }

                state_ = detail::allocate_shared_state<T>(resource_);
                auto handle = detail::coroutine_handle<PromiseDerived>::from_promise(
                    static_cast<PromiseDerived&>(*this));

                return unique_future<T>{handle, state_};
            }

            // suspend_never - immediate start (dispatch does co_await)
            detail::suspend_never initial_suspend() noexcept { return {}; }

            // final_suspend - self-destroy + symmetric transfer for method coroutines
            // NOTE: This is DIFFERENT from promise::set_value() which does NOT resume.
            // Method coroutines (unique_future<T>) run in the same actor context,
            // so symmetric transfer is safe and necessary for coroutine chaining.
            // Cross-actor futures (from send()) use promise::set_value() which spins.
            auto final_suspend() noexcept {
                struct final_awaiter {
                    state_type* state_;

                    bool await_ready() noexcept { return false; }

                    detail::coroutine_handle<> await_suspend(
                        detail::coroutine_handle<PromiseDerived> self) noexcept {
                        // 1. Take continuation FIRST (atomic exchange)
                        auto cont = state_->continuation_.exchange(nullptr,
                                                                    std::memory_order_acq_rel);

                        // 2. Set promise_finalizing flag BEFORE release_promise.
                        //    This prevents release_future() from deallocating while we're
                        //    still deciding whether to resume the continuation.
                        state_->flags_.fetch_or(detail::state_flags::promise_finalizing,
                                                std::memory_order_release);

                        // 3. Release promise. Returns true if future was already released
                        //    at the time of the atomic fetch_or.
                        bool cancelled = state_->release_promise();

                        if (cancelled) {
                            // Future was released before we set promise_released.
                            // State was deallocated by release_promise.
                            self.destroy();
                            return detail::noop_coroutine();
                        }

                        // 4. Try to complete finalize phase. Uses CAS to atomically
                        //    clear finalizing and check if future was released.
                        //    Returns false if future was released (state deallocated).
                        if (!state_->try_complete_finalize()) {
                            self.destroy();
                            return detail::noop_coroutine();
                        }

                        // 5. Consumer is still alive - symmetric transfer to continuation
                        // This is safe for method coroutines (same actor context)
                        self.destroy();
                        return cont ? cont : detail::noop_coroutine();
                    }

                    void await_resume() noexcept {}
                };
                return final_awaiter{this->state_};
            }

            // await_transform overloads (unique_future<U>&&, pair<bool, unique_future<U>>&&,
            // generator<U>&) and the constrained generic passthrough are inherited from
            // detail::future_awaiter_mixin<PromiseDerived>.

            void unhandled_exception() noexcept {
                assert(false && "unhandled_exception() should never be called (-fno-exceptions)");
            }

            // Default constructor
            promise_type_base() noexcept
                : resource_(nullptr)
                , state_(nullptr) {}

            template<typename First, typename... Args>
            promise_type_base(First&& first, Args&&... args) noexcept
                : resource_(extract_resource_or_abort(std::forward<First>(first), std::forward<Args>(args)...))
                , state_(nullptr) {}

            ~promise_type_base() noexcept = default;

            // propagate_awaited_state() / update_propagated_outer() / clear_awaited_chain()
            // and the awaited-chain fields are inherited from detail::future_awaiter_mixin.

        protected:
            template<typename U>
            static std::pmr::memory_resource* try_get_resource(U* ptr) noexcept {
                if constexpr (detail::has_resource_method<U>) {
                    return ptr->resource();
                } else {
                    return nullptr;
                }
            }

            template<typename U>
            static std::pmr::memory_resource* extract_resource_impl(U&& arg) noexcept {
                using decayed = std::decay_t<U>;
                if constexpr (std::is_pointer_v<decayed>) {
                    using ptr_type = std::remove_reference_t<U>;
                    return try_get_resource(static_cast<ptr_type>(arg));
                } else {
                    return try_get_resource(&arg);
                }
            }

            static std::pmr::memory_resource* extract_resource_impl(std::pmr::memory_resource* res) noexcept {
                return res;
            }

            static std::pmr::memory_resource* extract_resource_from_args() noexcept {
                return nullptr;
            }

            template<typename First, typename... Rest>
            static std::pmr::memory_resource* extract_resource_from_args(First&& first, Rest&&... rest) noexcept {
                auto res = extract_resource_impl(std::forward<First>(first));
                if (res != nullptr)
                    return res;
                if constexpr (sizeof...(Rest) > 0) {
                    return extract_resource_from_args(std::forward<Rest>(rest)...);
                }
                return nullptr;
            }

            [[noreturn]] static std::pmr::memory_resource* extract_resource_or_abort() noexcept {
                assert(false && "Coroutine must be defined inline (GCC doesn't pass 'this' for out-of-line methods)");
                std::abort();
            }

            template<typename First, typename... Rest>
            RETURNS_NONNULL static std::pmr::memory_resource* extract_resource_or_abort(First&& first, Rest&&... rest) noexcept {
                auto* res = extract_resource_from_args(std::forward<First>(first), std::forward<Rest>(rest)...);
                assert(res != nullptr && "Coroutine must be actor member function with resource() method");
                if (!res) {
                    std::abort();
                }
                return res;
            }
        };

        template<typename PromiseDerived>
        struct promise_type_non_void : promise_type_base<PromiseDerived> {
            using promise_type_base<PromiseDerived>::promise_type_base;

            void return_value(T&& value) noexcept {
                this->state_->set_value(std::forward<T>(value));
            }

            void return_value(const T& value) noexcept {
                this->state_->set_value(value);
            }

            void return_value(std::error_code ec) noexcept {
                this->state_->set_error(ec);
            }
        };

        template<typename PromiseDerived>
        struct promise_type_void : promise_type_base<PromiseDerived> {
            using promise_type_base<PromiseDerived>::promise_type_base;

            void return_void() noexcept {
                this->state_->set_value();
            }
        };

        template<typename PromiseDerived>
        using promise_type_selected = std::conditional_t<is_void_type, promise_type_void<PromiseDerived>, promise_type_non_void<PromiseDerived>>;

    public:
        // promise_type uses CRTP to pass itself to the base
        struct promise_type : promise_type_selected<promise_type> {
            using promise_type_selected<promise_type>::promise_type_selected;

            template<typename... Args>
            static void* operator new(std::size_t size, const Args&... args) {
                auto* res = promise_type_base<promise_type>::extract_resource_or_abort(args...);
                return detail::allocate_coro_frame(res, size);
            }

            template<typename... Args>
            static void operator delete(void* ptr, std::size_t size, const Args&...) noexcept {
                detail::deallocate_coro_frame(ptr, size);
            }

            static void operator delete(void* ptr, std::size_t size) noexcept {
                detail::deallocate_coro_frame(ptr, size);
            }

            static void operator delete(void* ptr) noexcept {
                detail::deallocate_coro_frame_unsized(ptr);
            }
        };

        // Promise for std::coroutine_traits specialization with explicit Actor& parameter.
        template<typename Actor>
        struct actor_promise : promise_type_selected<actor_promise<Actor>> {
            using base_type = promise_type_selected<actor_promise<Actor>>;

            template<typename... Args>
            actor_promise(Actor& actor, Args&&...) noexcept
                : base_type(actor.resource()) {}

            template<typename... Args>
            static void* operator new(std::size_t size, const Args&... args) {
                auto* res = promise_type_base<actor_promise>::extract_resource_or_abort(args...);
                return detail::allocate_coro_frame(res, size);
            }

            template<typename... Args>
            static void operator delete(void* ptr, std::size_t size, const Args&...) noexcept {
                detail::deallocate_coro_frame(ptr, size);
            }

            static void operator delete(void* ptr, std::size_t size) noexcept {
                detail::deallocate_coro_frame(ptr, size);
            }

            static void operator delete(void* ptr) noexcept {
                detail::deallocate_coro_frame_unsized(ptr);
            }
        };

    private:
        state_type* state_;
        detail::coroutine_handle<promise_type> handle_;
    };

    // Implementation of promise<T>::get_future()
    template<typename T>
    unique_future<T> promise<T>::get_future() noexcept {
        assert(state_ && "get_future() on moved-from promise");
        return unique_future<T>(state_);
    }

    // Free function co_await operator
    template<typename T>
    auto operator co_await(unique_future<T>&& f) noexcept {
        return typename unique_future<T>::awaiter{f.internal_state()};
    }

    // Factory functions
    template<typename T>
    [[nodiscard]] unique_future<T> make_error(std::pmr::memory_resource* res, std::error_code ec) {
        promise<T> p(res);
        auto f = p.get_future();  // Get future BEFORE error()
        p.error(ec);
        return f;
    }

    template<typename T>
    [[nodiscard]] unique_future<T> make_ready_future(std::pmr::memory_resource* res, T&& value) {
        assert(res && "make_ready_future: resource must not be null");
        promise<T> p(res);
        auto f = p.get_future();  // Get future BEFORE set_value
        p.set_value(std::forward<T>(value));
        return f;
    }

    [[nodiscard]] inline unique_future<void> make_ready_future(std::pmr::memory_resource* res) {
        assert(res && "make_ready_future: resource must not be null");
        promise<void> p(res);
        auto f = p.get_future();  // Get future BEFORE set_value
        p.set_value();
        return f;
    }

    template<typename T>
        requires(!std::is_void_v<T> && std::is_default_constructible_v<T>)
    [[nodiscard]] unique_future<T> make_ready_future(std::pmr::memory_resource* res) {
        assert(res && "make_ready_future: resource must not be null");
        promise<T> p(res);
        auto f = p.get_future();  // Get future BEFORE set_value
        p.set_value(T{});
        return f;
    }

} // namespace actor_zeta
