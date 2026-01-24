#pragma once

#include <cassert>
#include <concepts>
#include <memory_resource>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

#include <actor-zeta/config.hpp>
#include <actor-zeta/detail/coro_frame_header.hpp>
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
            // Take continuation BEFORE release_promise (which might deallocate state)
            auto cont = state_->take_continuation();
            state_->release_promise();
            state_ = nullptr;  // ownership transferred
            // Resume continuation AFTER state handling is complete
            // This ensures no use-after-free on state_ members
            if (cont && !cont.done()) {
                cont.resume();
            }
        }

        // Set value (void)
        void set_value() noexcept
            requires(std::is_void_v<T>)
        {
            assert(state_ && "set_value() on moved-from promise");
            state_->set_value();
            auto cont = state_->take_continuation();
            state_->release_promise();
            state_ = nullptr;
            if (cont && !cont.done()) {
                cont.resume();
            }
        }

        // Set error
        void set_error(std::error_code ec) noexcept {
            assert(state_ && "set_error() on moved-from promise");
            state_->set_error(ec);
            auto cont = state_->take_continuation();
            state_->release_promise();
            state_ = nullptr;
            if (cont && !cont.done()) {
                cont.resume();
            }
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
                auto cont = state_->take_continuation();
                state_->release_promise();
                state_ = nullptr;
                if (cont && !cont.done()) {
                    cont.resume();
                }
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

        // === Backport API (optional, for legacy code) ===

        // Smart wait - waits for promise_released (safe for destroy)
        void wait() const noexcept {
            if (!state_) return;

            auto flags = state_->flags_.load(std::memory_order_acquire);

            // Fast path
            if (flags & detail::state_flags::promise_released) {
                return;
            }

            // Slow path: exponential backoff
            int spin_count = 0;
            constexpr int spin_limit = 100;

            while (!(flags & detail::state_flags::promise_released)) {
                if (spin_count < spin_limit) {
                    ++spin_count;
#if defined(__x86_64__) || defined(_M_X64)
                    // _mm_pause() equivalent
                    asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
                    asm volatile("yield" ::: "memory");
#endif
                } else {
#if HAVE_ATOMIC_WAIT
                    state_->flags_.wait(flags, std::memory_order_acquire);
#else
                    std::this_thread::yield();
#endif
                }
                flags = state_->flags_.load(std::memory_order_acquire);
            }
        }

        // Blocking get - uses smart wait
        [[nodiscard]] T get() && requires(!std::is_void_v<T>) {
            assert(state_ && "get() on invalid future");
            wait();
            assert(!state_->has_error() && "future completed with error");
            T result = state_->take_value();
            release();
            return result;
        }

        void get() && requires(std::is_void_v<T>) {
            assert(state_ && "get() on invalid future");
            wait();
            assert(!state_->has_error() && "future completed with error");
            release();
        }

        T get() & requires(!std::is_void_v<T>) = delete;
        void get() & requires(std::is_void_v<T>) = delete;

        // is_ready for polling - checks promise_released
        [[nodiscard]] bool is_ready() const noexcept {
            return state_ && state_->is_ready();
        }

        // Alias for compatibility
        [[nodiscard]] bool available() const noexcept {
            return is_ready();
        }

        [[nodiscard]] bool valid() const noexcept {
            return state_ != nullptr;
        }

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

        // Cancel/ignore
        void ignore() noexcept {
            release();
        }

        // === Backward compatibility methods ===

        // is_cancelled() - check if the future was cancelled
        [[nodiscard]] bool is_cancelled() const noexcept {
            return state_ && state_->has_error() &&
                   state_->get_error() == std::make_error_code(std::errc::operation_canceled);
        }

        // cancel() - cancel the future (set error)
        void cancel() noexcept {
            if (state_ && !state_->has_result()) {
                state_->set_error(std::make_error_code(std::errc::operation_canceled));
            }
        }

        // Internal access (for message class)
        [[nodiscard]] state_type* internal_state() const noexcept {
            return state_;
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

        // CRTP base: PromiseDerived is the final promise type
        template<typename PromiseDerived>
        struct promise_type_base {
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

            // final_suspend - self-destroy + symmetric transfer
            auto final_suspend() noexcept {
                struct final_awaiter {
                    state_type* state_;

                    bool await_ready() noexcept { return false; }

                    detail::coroutine_handle<> await_suspend(
                        detail::coroutine_handle<PromiseDerived> self) noexcept {
                        // 1. Take continuation FIRST (atomic exchange)
                        auto cont = state_->continuation_.exchange(nullptr,
                                                                    std::memory_order_acq_rel);

                        // 2. Release promise (Last-One-Out, may deallocate state)
                        state_->release_promise();

                        // 3. Self-destroy coroutine
                        self.destroy();

                        // 4. Symmetric transfer to continuation (or noop if nobody waiting)
                        return cont ? cont : detail::noop_coroutine();
                    }

                    void await_resume() noexcept {}
                };
                return final_awaiter{this->state_};
            }

            template<typename U>
            auto await_transform(unique_future<U>&& future) noexcept {
                return typename unique_future<U>::awaiter{future.internal_state()};
            }

            // await_transform for pair<bool, unique_future<U>> from send()
            template<typename U>
            auto await_transform(std::pair<bool, unique_future<U>>&& p) noexcept {
                struct pair_awaiter {
                    bool needs_sched_;
                    typename unique_future<U>::awaiter inner_;

                    bool await_ready() const noexcept { return inner_.await_ready(); }

                    auto await_suspend(std::coroutine_handle<> h) noexcept {
                        return inner_.await_suspend(h);
                    }

                    std::pair<bool, U> await_resume() {
                        return {needs_sched_, inner_.await_resume()};
                    }
                };
                return pair_awaiter{p.first, typename unique_future<U>::awaiter{p.second.internal_state()}};
            }

            // await_transform for pair<bool, unique_future<void>> from send()
            auto await_transform(std::pair<bool, unique_future<void>>&& p) noexcept {
                struct void_pair_awaiter {
                    bool needs_sched_;
                    typename unique_future<void>::awaiter inner_;

                    bool await_ready() const noexcept { return inner_.await_ready(); }

                    auto await_suspend(std::coroutine_handle<> h) noexcept {
                        return inner_.await_suspend(h);
                    }

                    bool await_resume() {
                        inner_.await_resume();
                        return needs_sched_;
                    }
                };
                return void_pair_awaiter{p.first, typename unique_future<void>::awaiter{p.second.internal_state()}};
            }

            template<typename U>
            auto await_transform(generator<U>& gen) noexcept {
                return detail::next_awaiter<U>{gen.internal_state()};
            }

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
        auto f = p.get_future();  // Get future BEFORE set_error
        p.set_error(ec);
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
