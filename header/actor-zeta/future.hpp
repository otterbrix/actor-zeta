#pragma once

#include <actor-zeta/config.hpp>
#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/intrusive_ptr.hpp>
#include <actor-zeta/detail/rtt.hpp>

#include <cassert>
#include <chrono>
#include <thread>
#include <utility>
#include <iostream>

namespace actor_zeta {

#if HAVE_STD_COROUTINES
    template<typename T>
    struct future_awaiter;
#endif

    template<typename T>
    class promise final {
    public:
        promise() = delete;
        promise(const promise&) = delete;
        promise& operator=(const promise&) = delete;

        explicit promise(detail::future_state_base* slot, pmr::memory_resource* res) noexcept
            : slot_(slot)
            , resource_(res) {
            assert(slot_ && "promise constructed with null slot");
        }

        promise(promise&& other) noexcept
            : slot_(other.slot_)
            , resource_(other.resource_) {
            other.slot_ = nullptr;
        }

        promise& operator=(promise&& other) noexcept {
            if (this != &other) {
                slot_ = other.slot_;
                resource_ = other.resource_;
                other.slot_ = nullptr;
            }
            return *this;
        }

        ~promise() noexcept = default;

        void set_value(T&& value) {
            assert(slot_ && "set_value() on moved-from promise");

            if (slot_->is_cancelled()) {
                assert(false && "set_value() on orphaned/cancelled promise");
                return;
            }

            slot_->set_result_rtt(detail::rtt(resource_, std::forward<T>(value)));
        }

        void set_value(const T& value) {
            assert(slot_ && "set_value() on moved-from promise");

            if (slot_->is_cancelled()) {
                assert(false && "set_value() on orphaned/cancelled promise");
                return;
            }

            slot_->set_result_rtt(detail::rtt(resource_, value));
        }

        [[nodiscard]] bool is_valid() const noexcept {
            return slot_ != nullptr;
        }

        [[nodiscard]] detail::future_state_base* slot() const noexcept {
            return slot_;
        }

    private:
        detail::future_state_base* slot_;
        pmr::memory_resource* resource_;
    };

    template<>
    class promise<void> final {
    public:
        promise() = delete;
        promise(const promise&) = delete;
        promise& operator=(const promise&) = delete;

        explicit promise(detail::future_state_base* slot, pmr::memory_resource* res) noexcept
            : slot_(slot), resource_(res) {
            assert(slot_ && "promise<void> constructed with null slot");
        }

        promise(promise&& other) noexcept
            : slot_(other.slot_), resource_(other.resource_) {
            other.slot_ = nullptr;
        }

        promise& operator=(promise&& other) noexcept {
            if (this != &other) {
                slot_ = other.slot_;
                resource_ = other.resource_;
                other.slot_ = nullptr;
            }
            return *this;
        }

        ~promise() noexcept = default;

        void set_value() {
            assert(slot_ && "set_value() on moved-from promise<void>");

            if (slot_->is_cancelled()) {
                assert(false && "set_value() on orphaned/cancelled promise");
                return;
            }

            slot_->set_result_rtt(detail::rtt(resource_));  // Empty rtt for void
        }

        [[nodiscard]] bool is_valid() const noexcept {
            return slot_ != nullptr;
        }

        [[nodiscard]] detail::future_state_base* slot() const noexcept {
            return slot_;
        }

    private:
        detail::future_state_base* slot_;
        pmr::memory_resource* resource_;
    };

    template<typename T>
    class unique_future final {
    public:
        explicit unique_future(pmr::memory_resource* /*res*/) noexcept
            : state_(nullptr)
            , needs_scheduling_(false) {
        }

        explicit unique_future(detail::future_state<T>* state, bool needs_sched = false) noexcept
            : state_(state)
            , needs_scheduling_(needs_sched) {
        }

        unique_future(const unique_future&) = delete;
        unique_future& operator=(const unique_future&) = delete;

        unique_future(unique_future&& other) noexcept
            : state_(other.state_)
            , needs_scheduling_(other.needs_scheduling_) {
            other.state_ = nullptr;
            other.needs_scheduling_ = false;
        }

        unique_future& operator=(unique_future&& other) noexcept {
            if (this != &other) {
                if (state_ && !state_->is_ready()) {
                    state_->set_state(detail::future_state_enum::cancelled);
                }

                if (state_) {
                    state_->release();
                }

                state_ = other.state_;
                needs_scheduling_ = other.needs_scheduling_;

                other.state_ = nullptr;
                other.needs_scheduling_ = false;
            }
            return *this;
        }

        ~unique_future() noexcept {
            if (state_) {
                state_->release();
            }
        }

        T get() && {
            assert(state_ && "get() on invalid future");

            int spin_count = 0;
            constexpr int fast_spin_limit = 10;
            constexpr int yield_limit = 100;

            while (!state_->is_ready() && spin_count < fast_spin_limit) {
                if (state_->is_cancelled()) {
                    state_->release();
                    state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    std::terminate();
                }
                ++spin_count;
            }

            while (!state_->is_ready() && spin_count < yield_limit) {
                if (state_->is_cancelled()) {
                    state_->release();
                    state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    std::terminate();
                }
                std::this_thread::yield();
                ++spin_count;
            }

#if HAVE_ATOMIC_WAIT
            // C++20: Efficient blocking wait (zero CPU usage)
            // Uses futex (Linux), __ulock_wait (macOS), or WaitOnAddress (Windows)
            auto current = state_->state();
            while (current == detail::future_state_enum::pending) {
                if (state_->is_cancelled()) {
                    state_->release();
                    state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    std::terminate();
                }

                // Block until state changes (0% CPU)
                state_->wait(current);
                current = state_->state();
            }
#else
            // C++17 fallback: Exponential backoff polling
            auto backoff = std::chrono::microseconds(1);
            constexpr auto max_backoff = std::chrono::microseconds(100);

            while (!state_->is_ready()) {
                if (state_->is_cancelled()) {
                    state_->release();
                    state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    std::terminate();
                }

                std::this_thread::sleep_for(backoff);
                if (backoff < max_backoff) {
                    backoff *= 2;
                }
            }
#endif

            T result = state_->result().template get<T>(0);

            state_->release();
            state_ = nullptr;

            return result;
        }

        T get() & = delete;

        [[nodiscard]] bool is_ready() const noexcept {
            return state_ && state_->is_ready();
        }

        [[nodiscard]] bool valid() const noexcept {
            return state_ != nullptr;
        }

        [[nodiscard]] bool needs_scheduling() const noexcept {
            return needs_scheduling_;
        }

        void cancel() noexcept {
            if (state_) {
                state_->set_state(detail::future_state_enum::cancelled);
            }
        }

        [[nodiscard]] bool is_cancelled() const noexcept {
            return state_ && state_->is_cancelled();
        }

        [[nodiscard]] detail::future_state<T>* get_state() const noexcept {
            return state_;
        }

        [[nodiscard]] pmr::memory_resource* memory_resource() const noexcept {
            assert(state_ && "memory_resource() called on moved-from or invalid future");
            return state_->memory_resource();
        }

#if HAVE_STD_COROUTINES
        struct promise_type {
            using value_type = T;

            unique_future<T> get_return_object() {
                assert(resource_ != nullptr &&
                       "Coroutine must be actor member function with resource() method");

                void* mem = resource_->allocate(sizeof(detail::future_state<T>),
                                                alignof(detail::future_state<T>));
                state_ = new (mem) detail::future_state<T>(resource_);

                auto handle = detail::coroutine_handle<promise_type>::from_promise(*this);
                state_->set_coroutine(handle);

                return unique_future<T>(state_, false);
            }

            detail::suspend_never initial_suspend() noexcept {
                return {};
            }

            detail::suspend_always final_suspend() noexcept {
                return {};
            }

            void return_value(T&& value) noexcept {
                assert(state_ && "return_value() with null state");
                detail::rtt result(resource_, std::forward<T>(value));
                state_->set_result(std::move(result));
            }

            void return_value(const T& value) noexcept {
                assert(state_ && "return_value() with null state");
                detail::rtt result(resource_, value);
                state_->set_result(std::move(result));
            }

            void return_value(unique_future<T>&& ready_future) noexcept {
                assert(state_ && "return_value() with null state");
                assert(ready_future.valid() && "return_value() with invalid future");
                T value = std::move(ready_future).get();
                detail::rtt result(resource_, std::move(value));
                state_->set_result(std::move(result));
            }

            void unhandled_exception() noexcept {
                assert(false && "unhandled_exception() should never be called (-fno-exceptions)");
            }

            // Default constructor - standalone coroutines will get nullptr resource
            promise_type() noexcept
                : resource_(nullptr)
                , state_(nullptr) {
            }

            // Constructor for actor member functions - extracts resource from actor*
            // When coroutine is member function, compiler passes 'this' as first argument
            template<typename First, typename... Args>
            promise_type(First&& first, Args&&...) noexcept
                : resource_(extract_resource_impl(std::forward<First>(first)))
                , state_(nullptr) {
            }

            ~promise_type() noexcept = default;

        private:
            // SFINAE: Extract resource from actor pointer (if has resource() method)
            template<typename U>
            static auto try_get_resource(U* ptr, int) noexcept
                -> decltype(ptr->resource()) {
                return ptr->resource();
            }

            // Fallback: Return nullptr (will assert in get_return_object)
            template<typename U>
            static pmr::memory_resource* try_get_resource(U*, ...) noexcept {
                return nullptr;
            }

            // Extract resource from pointer type
            template<typename U>
            static pmr::memory_resource* extract_resource_impl(U* ptr) noexcept {
                return try_get_resource(ptr, 0);
            }

            // Fallback for non-pointer types -> nullptr
            template<typename U>
            static pmr::memory_resource* extract_resource_impl(U&&) noexcept {
                return nullptr;
            }

            pmr::memory_resource* resource_;
            detail::future_state<T>* state_;
        };
#endif

    private:
        detail::future_state<T>* state_;
        bool needs_scheduling_;
    };

    template<>
    class unique_future<void> final {
    public:
        explicit unique_future(pmr::memory_resource* /*res*/) noexcept
            : state_(nullptr)
            , needs_scheduling_(false)
            , is_ready_void_(false) {
        }

        explicit unique_future(detail::future_state<void>* state, bool needs_sched = false) noexcept
            : state_(state)
            , needs_scheduling_(needs_sched)
            , is_ready_void_(false) {
        }

        // ✅ Static factory for ready void future (zero allocation)
        static unique_future<void> make_ready() noexcept {
            unique_future<void> f(nullptr, false);
            f.is_ready_void_ = true;
            return f;
        }

        unique_future(const unique_future&) = delete;
        unique_future& operator=(const unique_future&) = delete;

        unique_future(unique_future&& other) noexcept
            : state_(other.state_)
            , needs_scheduling_(other.needs_scheduling_)
            , is_ready_void_(other.is_ready_void_) {
            other.state_ = nullptr;
            other.needs_scheduling_ = false;
            other.is_ready_void_ = false;
        }

        unique_future& operator=(unique_future&& other) noexcept {
            if (this != &other) {
                // ✅ Don't release if this is ready void (no allocation)
                if (!is_ready_void_) {
                    if (state_ && !state_->is_ready()) {
                        state_->set_state(detail::future_state_enum::cancelled);
                    }

                    if (state_) {
                        state_->release();
                    }
                }

                state_ = other.state_;
                needs_scheduling_ = other.needs_scheduling_;
                is_ready_void_ = other.is_ready_void_;

                other.state_ = nullptr;
                other.needs_scheduling_ = false;
                other.is_ready_void_ = false;
            }
            return *this;
        }

        ~unique_future() noexcept {
            // ✅ Don't release if this is ready void (no allocation)
            if (state_ && !is_ready_void_) {
                state_->release();
            }
        }

        void get() && {
            // ✅ Fast path for ready void (no allocation, no waiting)
            if (is_ready_void_) {
                return;
            }

            assert(state_ && "get() on invalid future");

            int spin_count = 0;
            constexpr int fast_spin_limit = 10;
            constexpr int yield_limit = 100;

            while (!state_->is_ready() && spin_count < fast_spin_limit) {
                if (state_->is_cancelled()) {
                    state_->release();
                    state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    return;
                }
                ++spin_count;
            }

            while (!state_->is_ready() && spin_count < yield_limit) {
                if (state_->is_cancelled()) {
                    state_->release();
                    state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    return;
                }
                std::this_thread::yield();
                ++spin_count;
            }

#if HAVE_ATOMIC_WAIT
            // C++20: Efficient blocking wait (zero CPU usage)
            auto current = state_->state();
            while (current == detail::future_state_enum::pending) {
                if (state_->is_cancelled()) {
                    state_->release();
                    state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    return;
                }

                // Block until state changes (0% CPU)
                state_->wait(current);
                current = state_->state();
            }
#else
            // C++17 fallback: Exponential backoff polling
            auto backoff = std::chrono::microseconds(1);
            constexpr auto max_backoff = std::chrono::microseconds(100);

            while (!state_->is_ready()) {
                if (state_->is_cancelled()) {
                    state_->release();
                    state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    return;
                }

                std::this_thread::sleep_for(backoff);
                if (backoff < max_backoff) {
                    backoff *= 2;
                }
            }
#endif

            state_->release();
            state_ = nullptr;
        }

        void get() & = delete;

        [[nodiscard]] bool is_ready() const noexcept {
            // ✅ Fast path for ready void
            return is_ready_void_ || (state_ && state_->is_ready());
        }

        [[nodiscard]] bool valid() const noexcept {
            // ✅ Ready void is valid even without state
            return is_ready_void_ || (state_ != nullptr);
        }

        [[nodiscard]] bool needs_scheduling() const noexcept {
            return needs_scheduling_;
        }

        void cancel() noexcept {
            if (state_) {
                state_->set_state(detail::future_state_enum::cancelled);
            }
        }

        [[nodiscard]] bool is_cancelled() const noexcept {
            return state_ && state_->is_cancelled();
        }

        [[nodiscard]] detail::future_state<void>* get_state() const noexcept {
            return state_;
        }

#if HAVE_STD_COROUTINES
        struct promise_type {
            using value_type = void;

            unique_future<void> get_return_object() {
                assert(resource_ != nullptr &&
                       "Coroutine must be actor member function with resource() method");

                void* mem = resource_->allocate(sizeof(detail::future_state<void>),
                                                alignof(detail::future_state<void>));
                state_ = new (mem) detail::future_state<void>(resource_);

                auto handle = detail::coroutine_handle<promise_type>::from_promise(*this);
                state_->set_coroutine(handle);

                return unique_future<void>(state_, false);
            }

            detail::suspend_never initial_suspend() noexcept { return {}; }

            detail::suspend_always final_suspend() noexcept { return {}; }

            void return_void() noexcept {
                assert(state_ && "return_void() with null state");
                state_->set_ready();
            }

            void unhandled_exception() noexcept {
                assert(false && "unhandled_exception() should never be called (-fno-exceptions)");
            }

            // Default constructor - standalone coroutines will get nullptr resource
            promise_type() noexcept
                : resource_(nullptr)
                , state_(nullptr) {
            }

            // Constructor for actor member functions - extracts resource from actor*
            template<typename First, typename... Args>
            promise_type(First&& first, Args&&...) noexcept
                : resource_(extract_resource_impl(std::forward<First>(first)))
                , state_(nullptr) {
            }

            ~promise_type() noexcept = default;

        private:
            // SFINAE: Extract resource from actor pointer (if has resource() method)
            template<typename U>
            static auto try_get_resource(U* ptr, int) noexcept
                -> decltype(ptr->resource()) {
                return ptr->resource();
            }

            // Fallback: Return nullptr (will assert in get_return_object)
            template<typename U>
            static pmr::memory_resource* try_get_resource(U*, ...) noexcept {
                return nullptr;
            }

            // Extract resource from pointer type
            template<typename U>
            static pmr::memory_resource* extract_resource_impl(U* ptr) noexcept {
                return try_get_resource(ptr, 0);
            }

            // Fallback for non-pointer types -> nullptr
            template<typename U>
            static pmr::memory_resource* extract_resource_impl(U&&) noexcept {
                return nullptr;
            }

            pmr::memory_resource* resource_;
            detail::future_state<void>* state_;
        };
#endif

    private:
        detail::future_state<void>* state_;
        bool needs_scheduling_;
        bool is_ready_void_;  // ✅ True for ready void future (zero allocation)
    };

    template<typename T>
    unique_future<T> make_ready_future(pmr::memory_resource* resource, T&& value) {
        void* mem = resource->allocate(sizeof(detail::future_state<T>),
                                       alignof(detail::future_state<T>));
        auto* state = new (mem) detail::future_state<T>(resource);

        detail::rtt result(resource, std::forward<T>(value));
        state->set_result(std::move(result));

        return unique_future<T>(state, false);
    }

    template<typename T>
    unique_future<T> make_ready_future(pmr::memory_resource* resource, const T& value) {
        void* mem = resource->allocate(sizeof(detail::future_state<T>),
                                       alignof(detail::future_state<T>));
        auto* state = new (mem) detail::future_state<T>(resource);

        detail::rtt result(resource, value);
        state->set_result(std::move(result));

        return unique_future<T>(state, false);
    }

    inline unique_future<void> make_ready_future_void(pmr::memory_resource* /*resource*/) {
        // ✅ Zero allocation - just return ready void future
        return unique_future<void>::make_ready();
    }

    template<typename T>
    unique_future<T> make_error_future(pmr::memory_resource* resource) {
        void* mem = resource->allocate(sizeof(detail::future_state<T>),
                                       alignof(detail::future_state<T>));
        auto* state = new (mem) detail::future_state<T>(resource);
        state->set_state(detail::future_state_enum::error);
        return unique_future<T>(state, false);
    }

}

#include <actor-zeta/detail/coroutine.hpp>

#if HAVE_STD_COROUTINES

namespace actor_zeta {

    template<typename T>
    struct future_awaiter {
        unique_future<T> future_;

        explicit future_awaiter(unique_future<T>&& f) noexcept
            : future_(std::move(f)) {
        }

        future_awaiter(const future_awaiter&) = delete;
        future_awaiter& operator=(const future_awaiter&) = delete;

        future_awaiter(future_awaiter&&) noexcept = default;
        future_awaiter& operator=(future_awaiter&&) noexcept = default;

        ~future_awaiter() noexcept = default;

        [[nodiscard]] bool await_ready() const noexcept {
            return future_.is_ready();
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            if (future_.is_ready()) {
                return false;
            }

            auto* state = future_.get_state();
            if (!state) {
                return false;
            }

            // ✅ CRITICAL: Save coroutine handle in future_state
            // Resume will be called from actor's behavior() via resume_all()
            // This ensures coroutine runs in CORRECT thread (actor's thread, not sender's)
            state->set_coroutine(handle);

            return true;
        }

        T await_resume() {
            assert(future_.valid() && "await_resume() on invalid future");
            return std::move(future_).get();
        }
    };

    template<>
    struct future_awaiter<void> {
        unique_future<void> future_;

        explicit future_awaiter(unique_future<void>&& f) noexcept
            : future_(std::move(f)) {
        }

        future_awaiter(const future_awaiter&) = delete;
        future_awaiter& operator=(const future_awaiter&) = delete;

        future_awaiter(future_awaiter&&) noexcept = default;
        future_awaiter& operator=(future_awaiter&&) noexcept = default;

        ~future_awaiter() noexcept = default;

        [[nodiscard]] bool await_ready() const noexcept {
            return future_.is_ready();
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            if (future_.is_ready()) {
                return false;
            }

            auto* state = future_.get_state();
            if (!state) {
                return false;
            }

            // ✅ CRITICAL: Save coroutine handle in future_state
            // Resume will be called from actor's behavior() via resume_all()
            // This ensures coroutine runs in CORRECT thread (actor's thread, not sender's)
            state->set_coroutine(handle);

            return true;
        }

        void await_resume() {
            assert(future_.valid() && "await_resume() on invalid future");
            std::move(future_).get();
        }
    };

    template<typename T>
    auto operator co_await(unique_future<T> f) noexcept {
        return future_awaiter<T>{std::move(f)};
    }

}

#endif