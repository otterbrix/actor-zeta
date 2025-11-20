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

        explicit promise(const intrusive_ptr<detail::future_state_base>& slot, pmr::memory_resource* res) noexcept
            : slot_(slot)
            , resource_(res) {
            assert(slot_ && "promise constructed with null slot");
        }

        promise(promise&& other) noexcept
            : slot_(std::move(other.slot_))
            , resource_(other.resource_) {
        }

        promise& operator=(promise&& other) noexcept {
            if (this != &other) {
                slot_ = std::move(other.slot_);
                resource_ = other.resource_;
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
            return slot_.get();
        }

    private:
        intrusive_ptr<detail::future_state_base> slot_;
        pmr::memory_resource* resource_;
    };

    template<>
    class promise<void> final {
    public:
        promise() = delete;
        promise(const promise&) = delete;
        promise& operator=(const promise&) = delete;

        explicit promise(const intrusive_ptr<detail::future_state_base>& slot, pmr::memory_resource* res) noexcept
            : slot_(slot), resource_(res) {
            assert(slot_ && "promise<void> constructed with null slot");
        }

        promise(promise&& other) noexcept
            : slot_(std::move(other.slot_)), resource_(other.resource_) {
        }

        promise& operator=(promise&& other) noexcept {
            if (this != &other) {
                slot_ = std::move(other.slot_);
                resource_ = other.resource_;
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
            return slot_.get();
        }

    private:
        intrusive_ptr<detail::future_state_base> slot_;
        pmr::memory_resource* resource_;
    };

    // Tag type for adopting an existing reference (no add_ref() in constructor)
    struct adopt_ref_t {};
    inline constexpr adopt_ref_t adopt_ref{};

    template<typename T>
    class unique_future final {
    public:
        explicit unique_future(pmr::memory_resource* /*res*/) noexcept
            : state_(nullptr)
            , needs_scheduling_(false) {
        }

        explicit unique_future(detail::future_state<T>* state, bool needs_sched = false) noexcept
            : state_(state)  // intrusive_ptr assignment calls add_ref() automatically
            , needs_scheduling_(needs_sched) {
        }

        // Constructor for adopting an existing reference (no add_ref())
        // Use this when the state already has a reference count for this future
        unique_future(adopt_ref_t, detail::future_state<T>* state, bool needs_sched = false) noexcept
            : state_(state, false)  // intrusive_ptr(ptr, false) adopts reference (no add_ref)
            , needs_scheduling_(needs_sched) {
        }

        unique_future(const unique_future&) = delete;
        unique_future& operator=(const unique_future&) = delete;

        unique_future(unique_future&& other) noexcept
            : state_(std::move(other.state_))  // intrusive_ptr move - no add_ref/release
            , needs_scheduling_(other.needs_scheduling_) {
            other.needs_scheduling_ = false;
        }

        unique_future& operator=(unique_future&& other) noexcept {
            if (this != &other) {
                if (state_ && !state_->is_ready()) {
                    state_->set_state(detail::future_state_enum::cancelled);
                }

                // intrusive_ptr move assignment handles release() + assignment atomically
                state_ = std::move(other.state_);
                needs_scheduling_ = other.needs_scheduling_;

                other.needs_scheduling_ = false;
            }
            return *this;
        }

        ~unique_future() noexcept {
            // intrusive_ptr destructor automatically calls release()
        }

        T get() && {
            assert(state_ && "get() on invalid future");

            int spin_count = 0;
            constexpr int fast_spin_limit = 10;
            constexpr int yield_limit = 100;

            while (!state_->is_ready() && spin_count < fast_spin_limit) {
                if (state_->is_cancelled()) {
                    state_ = nullptr;  // intrusive_ptr = nullptr calls release()
                    assert(false && "get() on cancelled future!");
                    std::terminate();
                }
                ++spin_count;
            }

            while (!state_->is_ready() && spin_count < yield_limit) {
                if (state_->is_cancelled()) {
                    state_ = nullptr;  // intrusive_ptr = nullptr calls release()
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
                    state_ = nullptr;  // intrusive_ptr = nullptr calls release()
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
                    state_ = nullptr;  // intrusive_ptr = nullptr calls release()
                    assert(false && "get() on cancelled future!");
                    std::terminate();
                }

                std::this_thread::sleep_for(backoff);
                if (backoff < max_backoff) {
                    backoff *= 2;
                }
            }
#endif

            // FIX: Use take_result() for atomic transition ready→consumed
            // Move result OUT of future_state before releasing
            detail::rtt result_rtt = state_->take_result();

            // CRITICAL: Release state IMMEDIATELY after taking result
            // This prevents race where message_guard::~message_guard() might also call release()
            // We no longer need state_ - all data is in local result_rtt
            // intrusive_ptr = nullptr automatically calls release()
            state_ = nullptr;

            // Safe: Extract value from LOCAL rtt, not from state
            T result = result_rtt.template get<T>(0);
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
            return state_.get();  // Return raw pointer from intrusive_ptr
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

                // Use adopt_ref - refcount starts at 1, future adopts that reference
                return unique_future<T>(adopt_ref, state_, false);
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
                : resource_(extract_resource_impl(first))
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

            // Helper: dispatch based on whether type is a pointer
            template<typename U>
            static pmr::memory_resource* extract_impl_dispatch(U&& arg, std::true_type /* is_pointer */) noexcept {
                // U is a pointer type - remove reference wrapper and pass as pointer
                // E.g., T*&& or T*& becomes T*
                using ptr_type = std::remove_reference_t<U>;
                return try_get_resource(static_cast<ptr_type>(arg), 0);
            }

            template<typename U>
            static pmr::memory_resource* extract_impl_dispatch(U&& arg, std::false_type /* not_pointer */) noexcept {
                // U is not a pointer - try to take address (for references)
                return try_get_resource(&arg, 0);
            }

            // Main entry point: handles all cases (pointer, reference, rvalue)
            template<typename U>
            static pmr::memory_resource* extract_resource_impl(U&& arg) noexcept {
                using decayed = std::decay_t<U>;
                return extract_impl_dispatch(std::forward<U>(arg), std::is_pointer<decayed>{});
            }

            pmr::memory_resource* resource_;
            detail::future_state<T>* state_;
        };
#endif

    private:
        intrusive_ptr<detail::future_state<T>> state_;  // Changed from raw pointer to intrusive_ptr
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
            : state_(state)  // intrusive_ptr assignment calls add_ref() automatically
            , needs_scheduling_(needs_sched)
            , is_ready_void_(false) {
        }

        // Constructor for adopting an existing reference (no add_ref())
        unique_future(adopt_ref_t, detail::future_state<void>* state, bool needs_sched = false) noexcept
            : state_(state, false)  // intrusive_ptr(ptr, false) adopts reference (no add_ref)
            , needs_scheduling_(needs_sched)
            , is_ready_void_(false) {
        }

        // Static factory for ready void future (zero allocation)
        static unique_future<void> make_ready() noexcept {
            unique_future<void> f(nullptr, false);
            f.is_ready_void_ = true;
            return f;
        }

        unique_future(const unique_future&) = delete;
        unique_future& operator=(const unique_future&) = delete;

        unique_future(unique_future&& other) noexcept
            : state_(std::move(other.state_))  // intrusive_ptr move - no add_ref/release
            , needs_scheduling_(other.needs_scheduling_)
            , is_ready_void_(other.is_ready_void_) {
            other.needs_scheduling_ = false;
            other.is_ready_void_ = false;
        }

        unique_future& operator=(unique_future&& other) noexcept {
            if (this != &other) {
                // Don't release if this is ready void (no allocation)
                if (!is_ready_void_) {
                    if (state_ && !state_->is_ready()) {
                        state_->set_state(detail::future_state_enum::cancelled);
                    }

                    // intrusive_ptr move assignment handles release() + assignment atomically
                    state_ = std::move(other.state_);
                } else {
                    state_ = std::move(other.state_);
                }

                needs_scheduling_ = other.needs_scheduling_;
                is_ready_void_ = other.is_ready_void_;

                other.needs_scheduling_ = false;
                other.is_ready_void_ = false;
            }
            return *this;
        }

        ~unique_future() noexcept {
            // intrusive_ptr destructor automatically calls release() (unless ready void with null state)
        }

        void get() && {
            // Fast path for ready void (no allocation, no waiting)
            if (is_ready_void_) {
                return;
            }

            assert(state_ && "get() on invalid future");

            int spin_count = 0;
            constexpr int fast_spin_limit = 10;
            constexpr int yield_limit = 100;

            while (!state_->is_ready() && spin_count < fast_spin_limit) {
                if (state_->is_cancelled()) {
                    state_ = nullptr;  // intrusive_ptr = nullptr calls release()
                    assert(false && "get() on cancelled future!");
                    return;
                }
                ++spin_count;
            }

            while (!state_->is_ready() && spin_count < yield_limit) {
                if (state_->is_cancelled()) {
                    state_ = nullptr;  // intrusive_ptr = nullptr calls release()
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
                    state_ = nullptr;  // intrusive_ptr = nullptr calls release()
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
                    state_ = nullptr;  // intrusive_ptr = nullptr calls release()
                    assert(false && "get() on cancelled future!");
                    return;
                }

                std::this_thread::sleep_for(backoff);
                if (backoff < max_backoff) {
                    backoff *= 2;
                }
            }
#endif

            // intrusive_ptr = nullptr automatically calls release()
            state_ = nullptr;
        }

        void get() & = delete;

        [[nodiscard]] bool is_ready() const noexcept {
            // Fast path for ready void
            return is_ready_void_ || (state_ && state_->is_ready());
        }

        [[nodiscard]] bool valid() const noexcept {
            // Ready void is valid even without state
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
            return state_.get();  // Return raw pointer from intrusive_ptr
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

                // Use adopt_ref - refcount starts at 1, future adopts that reference
                return unique_future<void>(adopt_ref, state_, false);
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
                : resource_(extract_resource_impl(first))
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

            // Helper: dispatch based on whether type is a pointer
            template<typename U>
            static pmr::memory_resource* extract_impl_dispatch(U&& arg, std::true_type /* is_pointer */) noexcept {
                // U is a pointer type - remove reference wrapper and pass as pointer
                // E.g., T*&& or T*& becomes T*
                using ptr_type = std::remove_reference_t<U>;
                return try_get_resource(static_cast<ptr_type>(arg), 0);
            }

            template<typename U>
            static pmr::memory_resource* extract_impl_dispatch(U&& arg, std::false_type /* not_pointer */) noexcept {
                // U is not a pointer - try to take address (for references)
                return try_get_resource(&arg, 0);
            }

            // Main entry point: handles all cases (pointer, reference, rvalue)
            template<typename U>
            static pmr::memory_resource* extract_resource_impl(U&& arg) noexcept {
                using decayed = std::decay_t<U>;
                return extract_impl_dispatch(std::forward<U>(arg), std::is_pointer<decayed>{});
            }

            pmr::memory_resource* resource_;
            detail::future_state<void>* state_;
        };
#endif

    private:
        intrusive_ptr<detail::future_state<void>> state_;  // Changed from raw pointer to intrusive_ptr
        bool needs_scheduling_;
        bool is_ready_void_;  // True for ready void future (zero allocation)
    };

    template<typename T>
    unique_future<T> make_ready_future(pmr::memory_resource* resource, T&& value) {
        void* mem = resource->allocate(sizeof(detail::future_state<T>),
                                       alignof(detail::future_state<T>));
        auto* state = new (mem) detail::future_state<T>(resource);  // refcount = 1

        detail::rtt result(resource, std::forward<T>(value));
        state->set_result(std::move(result));

        // Use adopt_ref - no message to release, future adopts initial refcount
        return unique_future<T>(adopt_ref, state, false);
    }

    template<typename T>
    unique_future<T> make_ready_future(pmr::memory_resource* resource, const T& value) {
        void* mem = resource->allocate(sizeof(detail::future_state<T>),
                                       alignof(detail::future_state<T>));
        auto* state = new (mem) detail::future_state<T>(resource);  // refcount = 1

        detail::rtt result(resource, value);
        state->set_result(std::move(result));

        // Use adopt_ref - no message to release, future adopts initial refcount
        return unique_future<T>(adopt_ref, state, false);
    }

    inline unique_future<void> make_ready_future_void(pmr::memory_resource* /*resource*/) {
        // Zero allocation - just return ready void future
        return unique_future<void>::make_ready();
    }

    template<typename T>
    unique_future<T> make_error_future(pmr::memory_resource* resource) {
        void* mem = resource->allocate(sizeof(detail::future_state<T>),
                                       alignof(detail::future_state<T>));
        auto* state = new (mem) detail::future_state<T>(resource);  // refcount = 1
        state->set_state(detail::future_state_enum::error);

        // Use adopt_ref - no message to release, future adopts initial refcount
        return unique_future<T>(adopt_ref, state, false);
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

            // CRITICAL: Save coroutine handle in future_state
            // Resume will be called from actor's behavior() via resume_all()
            // This ensures coroutine runs in CORRECT thread (actor's thread, not sender's)
            state->set_coroutine(handle);

            // FIX Problem #9: Re-check after set_coroutine() to close race window
            // Race scenario:
            //   1. We check is_ready() → false
            //   2. Another thread calls set_result(), sees has_coroutine() == false, doesn't resume
            //   3. We call set_coroutine(handle)
            //   4. → Nobody will call resume() → HANG FOREVER
            // Solution: Re-check is_ready() after setting handle
            // If ready now, return false (don't suspend) - coroutine will run immediately
            if (future_.is_ready()) {
                return false;  // Don't suspend - another thread set result
            }

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

            // CRITICAL: Save coroutine handle in future_state
            // Resume will be called from actor's behavior() via resume_all()
            // This ensures coroutine runs in CORRECT thread (actor's thread, not sender's)
            state->set_coroutine(handle);

            // FIX Problem #9: Re-check after set_coroutine() to close race window
            // Race scenario:
            //   1. We check is_ready() → false
            //   2. Another thread calls set_result(), sees has_coroutine() == false, doesn't resume
            //   3. We call set_coroutine(handle)
            //   4. → Nobody will call resume() → HANG FOREVER
            // Solution: Re-check is_ready() after setting handle
            // If ready now, return false (don't suspend) - coroutine will run immediately
            if (future_.is_ready()) {
                return false;  // Don't suspend - another thread set result
            }

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