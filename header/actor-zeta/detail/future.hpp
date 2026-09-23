#pragma once

#include <cassert>
#include <exception>
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

    template<typename T>
    class unique_future;

    // The user-facing promise for explicit promise/future pairs.
    template<typename T>
    class promise final {
    private:
        static constexpr bool is_void_type = std::is_void_v<T>;
        using state_type = detail::shared_state<T>;

    public:
        promise() = delete;
        promise(const promise&) = delete;
        promise& operator=(const promise&) = delete;

        explicit promise(std::pmr::memory_resource* res)
            : state_(detail::allocate_shared_state<T>(res)) {
            assert(res && "promise constructed with null resource");
        }

        // Non-owning view onto an existing state (message::get_result_promise).
        explicit promise(state_type* state) noexcept
            : state_(state) {}

        promise(promise&& other) noexcept
            : state_(std::exchange(other.state_, nullptr)) {}

        promise& operator=(promise&& other) noexcept {
            if (this != &other) {
                release_if_needed();
                state_ = std::exchange(other.state_, nullptr);
            }
            return *this;
        }

        ~promise() noexcept {
            release_if_needed();
        }

        [[nodiscard]] unique_future<T> get_future() noexcept;

        template<typename U>
            requires(!std::is_void_v<T> && std::is_constructible_v<T, U&&>)
        void set_value(U&& value) noexcept {
            assert(state_ && "set_value() on moved-from promise");
            state_->set_value(std::forward<U>(value));
            settle();
        }

        void set_value() noexcept
            requires(std::is_void_v<T>)
        {
            assert(state_ && "set_value() on moved-from promise");
            state_->set_value();
            settle();
        }

        // Also the cancellation channel: error(make_error_code(errc::operation_canceled)).
        void error(std::error_code ec) noexcept {
            assert(state_ && "error() on moved-from promise");
            state_->set_error(ec);
            settle();
        }

#ifdef __cpp_exceptions
        // Public: a router filling msg->get_result_promise<T>() by hand must pass a caught
        // exception on, not flatten it to a code. Also stamps errc::interrupted so failed()/
        // error() pollers see a real failure. Guarded: with -fno-exceptions nothing reaches here.
        void exception(std::exception_ptr ep) noexcept {
            assert(state_ && "exception() on moved-from promise");
            assert(ep && "exception() with a null exception_ptr");
            state_->set_exception(ep);
            settle();
        }
#endif

        [[nodiscard]] bool valid() const noexcept {
            return state_ != nullptr;
        }

        [[nodiscard]] state_type* internal_state() const noexcept {
            return state_;
        }

    private:
        void release_if_needed() noexcept {
            if (state_) {
                // Dying without an outcome still produces one; broken_pipe says more than state_not_recoverable.
                state_->set_error(std::make_error_code(std::errc::broken_pipe));
                settle();
            }
        }

        // The producer's exit. The continuation is NOT taken or resumed here -- the
        // consumer's own resume_impl() does that, on its own thread. promise_finalizing
        // BEFORE release_promise(), or a concurrent release_future() deallocates under us.
        void settle() noexcept {
            assert(state_ && "settle() without a state");

            state_->flags_.fetch_or(detail::state_flags::promise_finalizing, std::memory_order_release);

            // true means release_promise() deallocated -- no second call.
            if (!state_->release_promise()) {
                [[maybe_unused]] const bool state_still_alive = state_->try_complete_finalize();
            }

            state_ = nullptr;
        }

        state_type* state_;
    };

    template<typename T>
    class unique_future final {
    public:
        // Must be public: coroutine_traits looks it up.
        struct promise_type;

    private:
        static constexpr bool is_void_type = std::is_void_v<T>;
        using state_type = detail::shared_state<T>;

    public:
        unique_future(const unique_future&) = delete;
        unique_future& operator=(const unique_future&) = delete;

        unique_future() noexcept
            : state_(nullptr)
            , handle_{} {}

        explicit unique_future(state_type* s) noexcept
            : state_(s)
            , handle_{} {}

        unique_future(detail::coroutine_handle<promise_type> h, state_type* s) noexcept
            : state_(s)
            , handle_(h) {}

        unique_future(unique_future&& other) noexcept
            : state_(std::exchange(other.state_, nullptr))
            , handle_(std::exchange(other.handle_, {})) {}

        unique_future& operator=(unique_future&& other) noexcept {
            if (this != &other) {
                release();
                state_ = std::exchange(other.state_, nullptr);
                handle_ = std::exchange(other.handle_, {});
            }
            return *this;
        }

        ~unique_future() noexcept {
            release();
        }

        // Asserts readiness instead of waiting for it.
        // Here rather than in result_storage, so the refusal covers every T:
        // result_storage<void> has no value to guard, and a failed void operation
        // would otherwise report success once NDEBUG removes the assert.
        void check_extractable() const {
            assert(state_ && "take_ready() on a moved-from future");
            state_->rethrow_if_exception();   // before has_error(): a captured exception sets it
            if (!state_->has_result() || state_->has_error()) {
                detail::refuse_valueless_extraction("take_ready()");
            }
        }

        [[nodiscard]] T take_ready() && requires(!std::is_void_v<T>) {
            check_extractable();
            T r = state_->take_value();
            release();
            return r;
        }
        void take_ready() && requires(std::is_void_v<T>) {
            check_extractable();
            state_->take_value();   // marks consumed, so holds_value() means one thing for every T
            release();
        }

        // Poll. promise_released, which a valueless death also sets: NOT a value gate, check failed() first.
        [[nodiscard]] bool is_ready() const noexcept {
            return state_ && state_->is_ready();
        }

        [[nodiscard]] bool valid() const noexcept {
            return state_ != nullptr;
        }

        // failed()/error() observe cancellation, produced by promise<T>::error(operation_canceled).
        [[nodiscard]] bool failed() const noexcept {
            return state_ && state_->has_error();
        }

        [[nodiscard]] std::error_code error() const noexcept {
            return state_ ? state_->get_error() : std::error_code{};
        }

        void detach() noexcept {
            release();
        }

        [[nodiscard]] state_type* internal_state() const noexcept {
            return state_;
        }

        // The producing coroutine's handle, for propagate_awaited_state() and for external
        // drivers that hand-roll the drain (examples/external-drive). The frame is OWNED by
        // this future -- a finished producer parks at final_suspend and release() reclaims it --
        // so the handle cannot be destroyed under the caller. Withheld once the state is ready:
        // resuming a coroutine at its final suspend point is undefined ([coroutine.handle.resumption]).
        // Empty is also normal for a promise<T>-backed future. Serialization is the caller's.
        [[nodiscard]] detail::coroutine_handle<promise_type> coroutine_handle() const noexcept {
            if (!state_ || state_->is_ready()) {
                return {};
            }
            return handle_;
        }

    private:
        void release() noexcept {
            // Reclaim the frame BEFORE releasing the state: done() means parked at final_suspend
            // and ours; mid-body it is the producer's, whose final_awaiter (steps 3/4) destroys it
            // on seeing future_released. After release_future(), done() could read freed memory.
            if (handle_ && handle_.done()) {
                handle_.destroy();
            }
            handle_ = {};

            if (state_) {
                state_->release_future();
                state_ = nullptr;
            }
        }

        // CRTP base: PromiseDerived is the final promise type.
        template<typename PromiseDerived>
        struct promise_type_base : detail::future_awaiter_mixin<PromiseDerived> {
            using value_type = T;

            std::pmr::memory_resource* resource_ = nullptr;
            state_type* state_ = nullptr;

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

            // Immediate start; dispatch() co_awaits the result.
            detail::suspend_never initial_suspend() noexcept { return {}; }

            // Symmetric transfer, UNLIKE promise::set_value(), which never resumes: a method
            // coroutine runs in its awaiter's actor context, so resuming here is safe and is
            // what makes chaining work. send() futures complete flag-only; the drain picks them up.
            auto final_suspend() noexcept {
                struct final_awaiter {
                    state_type* state_;

                    bool await_ready() noexcept { return false; }

                    detail::coroutine_handle<> await_suspend(
                        detail::coroutine_handle<PromiseDerived> self) noexcept {
                        // 1. Take the continuation FIRST.
                        auto cont = state_->continuation_.exchange(nullptr,
                                                                    std::memory_order_acq_rel);

                        // 2. Finalizing BEFORE release_promise, or release_future() may deallocate mid-decision.
                        state_->flags_.fetch_or(detail::state_flags::promise_finalizing,
                                                std::memory_order_release);

                        // 3. true: the future was already released, the state is gone, the frame is ours.
                        bool cancelled = state_->release_promise();

                        if (cancelled) {
                            self.destroy();
                            return detail::noop_coroutine();
                        }

                        // 4. Clear finalizing; false means future_released raced in and the state is gone.
                        if (!state_->try_complete_finalize()) {
                            self.destroy();
                            return detail::noop_coroutine();
                        }

                        // 5. Consumer alive: do NOT destroy -- its unique_future holds this handle
                        //    and reclaims it in release(). Steps 3/4 must, since nobody else would.
                        return cont ? cont : detail::noop_coroutine();
                    }

                    void await_resume() noexcept {}
                };
                return final_awaiter{this->state_};
            }

            void unhandled_exception() noexcept {
#ifdef __cpp_exceptions
                // Returning from here means "handled" -- the exception would be swallowed.
                // Capture it; take_ready() and await_resume() rethrow where the value would be.
                if (this->state_) {
                    this->state_->set_exception(std::current_exception());
                }
#else
                // Unreachable without a catch wrapper; the promise concept requires the member.
                std::terminate();
#endif
            }

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

            // The runtime path's predicate, lifted so the static_assert can never disagree with it.
            template<typename U>
            static constexpr bool supplies_resource() noexcept {
                using decayed = std::decay_t<U>;
                if constexpr (std::is_same_v<decayed, std::pmr::memory_resource*>) {
                    return true;
                } else if constexpr (std::is_pointer_v<decayed>) {
                    return detail::has_resource_method<
                        std::remove_pointer_t<std::remove_reference_t<U>>>;
                } else {
                    return detail::has_resource_method<std::remove_reference_t<U>>;
                }
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

            // Zero arguments: nothing can carry a resource, a property of the SIGNATURE.
            // Dependent on a defaulted parameter so it fires only when this overload is selected.
            template<typename Dependent = PromiseDerived>
            [[noreturn]] static std::pmr::memory_resource* extract_resource_or_abort() noexcept {
                static_assert(sizeof(Dependent) == 0,
                              "a unique_future<T> coroutine must be an actor member function "
                              "defined inline (GCC does not pass 'this' for out-of-line methods), "
                              "or take a std::pmr::memory_resource* argument");
                std::abort();
            }

            template<typename First, typename... Rest>
            RETURNS_NONNULL static std::pmr::memory_resource* extract_resource_or_abort(First&& first, Rest&&... rest) noexcept {
                // Type-level: can ANY argument supply a resource? A null at run time is the assert's job.
                static_assert((supplies_resource<First>() || ... || supplies_resource<Rest>()),
                              "no argument of this coroutine can supply a memory resource -- "
                              "make it an actor member function (so `this` is in the pack) or "
                              "pass a std::pmr::memory_resource*");
                auto* res = extract_resource_from_args(std::forward<First>(first), std::forward<Rest>(rest)...);
                assert(res != nullptr && "resource() returned null");
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

    template<typename T>
    unique_future<T> promise<T>::get_future() noexcept {
        assert(state_ && "get_future() on moved-from promise");
        return unique_future<T>(state_);
    }

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
