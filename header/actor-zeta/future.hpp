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

namespace actor_zeta {

    // Forward declaration for awaiter (used by promise::await_transform)
    template<typename T>
    struct awaiter;


    // Forward declaration
    template<typename T>
    class unique_future;

    template<typename T>
    class promise final {
    public:
        promise() = delete;
        promise(const promise&) = delete;
        promise& operator=(const promise&) = delete;

        /// @brief Create promise that owns its state (Seastar-style)
        /// @param res Memory resource for state allocation
        explicit promise(pmr::memory_resource* res)
            : state_(nullptr)
            , resource_(res)
            , owns_state_(true) {
            assert(res && "promise constructed with null resource");
            void* mem = resource_->allocate(sizeof(detail::future_state<T>),
                                            alignof(detail::future_state<T>));
            state_ = new (mem) detail::future_state<T>(resource_);
            // state_ starts with refcount=1, promise owns it
        }

        promise(promise&& other) noexcept
            : state_(other.state_)
            , resource_(other.resource_)
            , owns_state_(other.owns_state_) {
            other.state_ = nullptr;
            other.owns_state_ = false;
        }

        promise& operator=(promise&& other) noexcept {
            if (this != &other) {
                release();
                state_ = other.state_;
                resource_ = other.resource_;
                owns_state_ = other.owns_state_;
                other.state_ = nullptr;
                other.owns_state_ = false;
            }
            return *this;
        }

        ~promise() noexcept {
            release();
        }

        /// @brief Get future associated with this promise (Seastar-style)
        /// @return unique_future that shares state with this promise
        /// @note Can be called multiple times (each future adds reference)
        [[nodiscard]] unique_future<T> get_future() noexcept;

        void set_value(T&& value) {
            assert(state_ && "set_value() on moved-from promise");

            if (state_->is_cancelled()) {
                assert(false && "set_value() on orphaned/cancelled promise");
                return;
            }

            state_->set_result_rtt(detail::rtt(resource_, std::forward<T>(value)));
        }

        void set_value(const T& value) {
            assert(state_ && "set_value() on moved-from promise");

            if (state_->is_cancelled()) {
                assert(false && "set_value() on orphaned/cancelled promise");
                return;
            }

            state_->set_result_rtt(detail::rtt(resource_, value));
        }

        [[nodiscard]] bool is_valid() const noexcept {
            return state_ != nullptr;
        }

        [[nodiscard]] detail::future_state<T>* state() const noexcept {
            return state_;
        }

        [[nodiscard]] pmr::memory_resource* resource() const noexcept {
            return resource_;
        }

        /// @brief Wrap existing state (non-owning) - for result propagation
        /// @param state Existing future state to wrap
        /// @param res Memory resource (required)
        /// @note State must outlive this promise. Caller responsible for lifetime.
        static promise wrap(detail::future_state<T>* state, pmr::memory_resource* res) noexcept {
            assert(res && "wrap() requires memory_resource");
            assert(state && "wrap() requires valid state");
            return promise(state, res, false);  // non-owning
        }

    private:
        // Private constructor for wrap() - non-owning
        promise(detail::future_state<T>* state, pmr::memory_resource* res, bool owns) noexcept
            : state_(state)
            , resource_(res)
            , owns_state_(owns) {}

        void release() noexcept {
            if (owns_state_ && state_) {
                intrusive_ptr_release(state_);
            }
            state_ = nullptr;
        }

        detail::future_state<T>* state_;
        pmr::memory_resource* resource_;
        bool owns_state_;
    };

    template<>
    class promise<void> final {
    public:
        promise() = delete;
        promise(const promise&) = delete;
        promise& operator=(const promise&) = delete;

        /// @brief Create promise that owns its state (Seastar-style)
        /// @param res Memory resource for state allocation
        explicit promise(pmr::memory_resource* res)
            : state_(nullptr)
            , resource_(res)
            , owns_state_(true) {
            assert(res && "promise<void> constructed with null resource");
            void* mem = resource_->allocate(sizeof(detail::future_state<void>),
                                            alignof(detail::future_state<void>));
            state_ = new (mem) detail::future_state<void>(resource_);
            // state_ starts with refcount=1, promise owns it
        }

        promise(promise&& other) noexcept
            : state_(other.state_)
            , resource_(other.resource_)
            , owns_state_(other.owns_state_) {
            other.state_ = nullptr;
            other.owns_state_ = false;
        }

        promise& operator=(promise&& other) noexcept {
            if (this != &other) {
                release();
                state_ = other.state_;
                resource_ = other.resource_;
                owns_state_ = other.owns_state_;
                other.state_ = nullptr;
                other.owns_state_ = false;
            }
            return *this;
        }

        ~promise() noexcept {
            release();
        }

        /// @brief Get future associated with this promise (Seastar-style)
        /// @return unique_future that shares state with this promise
        /// @note Can be called multiple times (each future adds reference)
        [[nodiscard]] unique_future<void> get_future() noexcept;

        void set_value() {
            assert(state_ && "set_value() on moved-from promise<void>");

            if (state_->is_cancelled()) {
                assert(false && "set_value() on orphaned/cancelled promise");
                return;
            }

            state_->set_result_rtt(detail::rtt(resource_));  // Empty rtt for void
        }

        [[nodiscard]] bool is_valid() const noexcept {
            return state_ != nullptr;
        }

        [[nodiscard]] detail::future_state<void>* state() const noexcept {
            return state_;
        }

        [[nodiscard]] pmr::memory_resource* resource() const noexcept {
            return resource_;
        }

        /// @brief Wrap existing state (non-owning) - for result propagation
        /// @param state Existing future state to wrap
        /// @param res Memory resource (required)
        /// @note State must outlive this promise. Caller responsible for lifetime.
        static promise wrap(detail::future_state<void>* state, pmr::memory_resource* res) noexcept {
            assert(res && "wrap() requires memory_resource");
            assert(state && "wrap() requires valid state");
            return promise(state, res, false);  // non-owning
        }

    private:
        // Private constructor for wrap() - non-owning
        promise(detail::future_state<void>* state, pmr::memory_resource* res, bool owns) noexcept
            : state_(state)
            , resource_(res)
            , owns_state_(owns) {}

        void release() noexcept {
            if (owns_state_ && state_) {
                intrusive_ptr_release(state_);
            }
            state_ = nullptr;
        }

        detail::future_state<void>* state_;
        pmr::memory_resource* resource_;
        bool owns_state_;
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

        void set_needs_scheduling(bool value) noexcept {
            needs_scheduling_ = value;
        }

        void cancel() noexcept {
            if (state_) {
                state_->set_state(detail::future_state_enum::cancelled);
            }
        }

        [[nodiscard]] bool is_cancelled() const noexcept {
            return state_ && state_->is_cancelled();
        }

        // === Coroutine control API (analogous to std::coroutine_handle) ===

        /// @brief Check if this future has an associated coroutine
        /// @return true if coroutine handle is stored (analogous to coroutine_handle::operator bool())
        [[nodiscard]] bool has_coroutine() const noexcept {
            return state_ && state_->has_coroutine();
        }

        /// @brief Check if associated coroutine is done (at final suspend)
        /// @return true if coroutine exists and completed (analogous to coroutine_handle::done())
        [[nodiscard]] bool coroutine_done() const noexcept {
            return state_ && state_->coroutine_done();
        }

        /// @brief Resume associated coroutine
        /// @note Analogous to coroutine_handle::resume()
        /// @note Caller must ensure awaiting_ready() == true before calling
        void resume() noexcept {
            if (state_) {
                state_->resume_coroutine();
            }
        }

        /// @brief Check if awaited future is ready
        /// @return true if coroutine is waiting on a future and that future is ready
        /// @note User code should call resume() when this returns true
        [[nodiscard]] bool awaiting_ready() const noexcept {
            if (!state_) return false;
            auto* awaiting = state_->get_awaiting_on();
            return awaiting && awaiting->is_ready();
        }

        [[nodiscard]] detail::future_state<T>* get_state() const noexcept {
            return state_.get();  // Return raw pointer from intrusive_ptr
        }

        /// @brief Release ownership of state (transfer to continuation chain)
        /// @return Raw pointer to state (caller takes ownership via refcount)
        /// @note After this call, future is INVALID (state_ = nullptr)
        /// @note Uses intrusive_ptr::detach() which does NOT decrement refcount
        /// @note Thread-safe: atomic operation without race condition
        ///
        /// Example usage:
        /// @code
        /// auto* raw_state = future.release_state();  // Ownership transferred
        /// raw_state->set_continuation(target);       // Link states
        /// // raw_state now owned by continuation chain (refcount unchanged)
        /// @endcode
        [[nodiscard]] detail::future_state<T>* release_state() noexcept {
            return state_.detach();  // intrusive_ptr::detach() returns ptr without decrement
        }

        [[nodiscard]] pmr::memory_resource* memory_resource() const noexcept {
            assert(state_ && "memory_resource() called on moved-from or invalid future");
            return state_->memory_resource();
        }

        // ============================================================================
        // Result propagation API (Seastar-style forward_to)
        // ============================================================================

        /// @brief Forward result from this future to target promise when ready
        /// @param target Promise to receive the result
        /// @note If already ready, copies result immediately
        /// @note If not ready, user must poll and call forward_to_if_ready() or copy manually
        /// @note This is USER-CONTROLLED - no automatic callbacks (thread affinity)
        void forward_to(promise<T>& target) {
            if (!state_) return;

            if (state_->is_ready()) {
                // Ready - copy result immediately
                target.set_value(std::move(*this).get());
            }
            // Not ready - user must poll and forward later
            // We don't set up automatic forwarding (violates thread affinity)
        }

        /// @brief Check if ready and forward to target if so
        /// @param target Promise to receive the result
        /// @return true if forwarded (was ready), false if still pending
        /// @note Call this in poll loop until it returns true
        bool forward_to_if_ready(promise<T>& target) {
            if (!state_ || !state_->is_ready()) {
                return false;
            }
            target.set_value(std::move(*this).get());
            return true;
        }

        struct promise_type {
            using value_type = T;

            unique_future<T> get_return_object() {
                assert(resource_ != nullptr &&
                       "Coroutine must be actor member function with resource() method");

                void* mem = resource_->allocate(sizeof(detail::future_state<T>),
                                                alignof(detail::future_state<T>));
                state_ = new (mem) detail::future_state<T>(resource_);


                // Save coroutine handle in promise state for Manual Polling
                // Actor can extract it via extract_coroutine_handle() and resume later
                // NOTE: This is safe because automatic resume in set_result_rtt() is disabled
                auto handle = std::coroutine_handle<promise_type>::from_promise(*this);
                state_->set_coroutine(handle);  // Implicit conversion to coroutine_handle<void>


                // Use adopt_ref - refcount starts at 1, future adopts that reference
                return unique_future<T>(adopt_ref, state_, false);
            }

            detail::suspend_never initial_suspend() noexcept {
                // Start immediately - coroutine runs until first co_await or co_return
                return {};
            }

            detail::suspend_always final_suspend() noexcept {
                return {};
            }

            // CRITICAL: await_transform enables continuation chain for nested co_await
            // When coroutine does `co_await send(other_actor, ...)`, we need to link:
            // awaited_future (from send) → promise_state (this coroutine's return value)
            // This ensures result propagates correctly through the chain.
            template<typename U>
            auto await_transform(unique_future<U>&& future) noexcept {
                return awaiter<U>{std::move(future), state_};
            }

            void return_value(T&& value) noexcept {
                assert(state_ && "return_value() with null state");
                detail::rtt result(resource_, std::forward<T>(value));
                state_->set_result_rtt(std::move(result));  // FIX: Use set_result_rtt() for continuation propagation!
            }

            void return_value(const T& value) noexcept {
                assert(state_ && "return_value() with null state");
                detail::rtt result(resource_, value);
                state_->set_result_rtt(std::move(result));  // FIX: Use set_result_rtt() for continuation propagation!
            }

            void return_value(unique_future<T>&& ready_future) noexcept {
                assert(state_ && "return_value() with null state");
                assert(ready_future.valid() && "return_value() with invalid future");
                T value = std::move(ready_future).get();
                detail::rtt result(resource_, std::move(value));
                state_->set_result_rtt(std::move(result));  // FIX: Use set_result_rtt() for continuation propagation!
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

        /// @brief Converting constructor from unique_future<T> (any T) - TRUE MOVE SEMANTICS
        /// @details Moves ownership from typed future to void future. After this call,
        ///          the original typed future is INVALID (state_ = nullptr).
        ///          The typed result is preserved in the state (via RTT), but this future
        ///          acts as void (cannot call .get() to retrieve typed value).
        ///          Continuation chain works correctly - typed result propagates through RTT.
        /// @note This enables returning unique_future<T> from dispatch() and converting
        ///       to unique_future<void> in behavior(), solving the return type conflict.
        /// @warning After this call, `other` is in moved-from state and MUST NOT be used!
        template<typename T>
        unique_future(unique_future<T>&& other) noexcept
            : state_(static_cast<detail::future_state<void>*>(
                  static_cast<detail::future_state_base*>(other.release_state())), false)
            , needs_scheduling_(other.needs_scheduling())
            , is_ready_void_(false) {
            // FIX: Use static_cast through future_state_base instead of reinterpret_cast
            // - release_state() returns raw pointer WITHOUT decrementing refcount
            // - Cast through base class (future_state_base*) is well-defined
            // - intrusive_ptr(ptr, false) adopts the reference (no add_ref)
            // - other.state_ is now nullptr (truly moved)
            //
            // This is safe because:
            // 1. Both future_state<T> and future_state<void> inherit from future_state_base
            // 2. We only call base class methods through this pointer (continuation, coroutine)
            // 3. Typed result stored in RTT (type-erased), continuation chain works correctly
            // 4. Never call typed methods on void future (no .get() that extracts typed value)
            //
            // IMPORTANT: other is now in moved-from state!
            other.set_needs_scheduling(false);
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

        void set_needs_scheduling(bool value) noexcept {
            needs_scheduling_ = value;
        }

        void cancel() noexcept {
            if (state_) {
                state_->set_state(detail::future_state_enum::cancelled);
            }
        }

        [[nodiscard]] bool is_cancelled() const noexcept {
            return state_ && state_->is_cancelled();
        }

        // === Coroutine control API (analogous to std::coroutine_handle) ===

        /// @brief Check if this future has an associated coroutine
        /// @return true if coroutine handle is stored (analogous to coroutine_handle::operator bool())
        [[nodiscard]] bool has_coroutine() const noexcept {
            return state_ && state_->has_coroutine();
        }

        /// @brief Check if associated coroutine is done (at final suspend)
        /// @return true if coroutine exists and completed (analogous to coroutine_handle::done())
        [[nodiscard]] bool coroutine_done() const noexcept {
            return state_ && state_->coroutine_done();
        }

        /// @brief Resume associated coroutine
        /// @note Analogous to coroutine_handle::resume()
        /// @note Caller must ensure awaiting_ready() == true before calling
        void resume() noexcept {
            if (state_) {
                state_->resume_coroutine();
            }
        }

        /// @brief Check if awaited future is ready
        /// @return true if coroutine is waiting on a future and that future is ready
        /// @note User code should call resume() when this returns true
        [[nodiscard]] bool awaiting_ready() const noexcept {
            if (!state_) return false;
            auto* awaiting = state_->get_awaiting_on();
            return awaiting && awaiting->is_ready();
        }

        [[nodiscard]] detail::future_state<void>* get_state() const noexcept {
            return state_.get();  // Return raw pointer from intrusive_ptr
        }

        /// @brief Release ownership of state (transfer to continuation chain)
        /// @return Raw pointer to state (caller takes ownership via refcount)
        /// @note After this call, future is INVALID (state_ = nullptr)
        /// @note Uses intrusive_ptr::detach() which does NOT decrement refcount
        [[nodiscard]] detail::future_state<void>* release_state() noexcept {
            return state_.detach();  // intrusive_ptr::detach() returns ptr without decrement
        }

        struct promise_type {
            using value_type = void;

            unique_future<void> get_return_object() {
                assert(resource_ != nullptr &&
                       "Coroutine must be actor member function with resource() method");

                void* mem = resource_->allocate(sizeof(detail::future_state<void>),
                                                alignof(detail::future_state<void>));
                state_ = new (mem) detail::future_state<void>(resource_);

                // Save coroutine handle in promise state for Manual Polling
                // Actor can extract it via extract_coroutine_handle() and resume later
                // NOTE: This is safe because automatic resume in set_result_rtt() is disabled
                auto handle = std::coroutine_handle<promise_type>::from_promise(*this);
                state_->set_coroutine(handle);  // Implicit conversion to coroutine_handle<void>

                // Use adopt_ref - refcount starts at 1, future adopts that reference
                return unique_future<void>(adopt_ref, state_, false);
            }

            detail::suspend_never initial_suspend() noexcept {
                // Start immediately - coroutine runs until first co_await or co_return
                return {};
            }

            detail::suspend_always final_suspend() noexcept { return {}; }

            // Enable co_await inside void coroutines (same as typed version)
            // This allows: unique_future<void> method() { co_await send(...); co_return; }
            template<typename U>
            auto await_transform(unique_future<U>&& future) noexcept {
                return awaiter<U>{std::move(future), state_};
            }

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

    // ============================================================================
    // promise<T>::get_future() implementation (Seastar-style)
    // ============================================================================

    template<typename T>
    unique_future<T> promise<T>::get_future() noexcept {
        assert(state_ && "get_future() on moved-from promise");
        // Future shares state with promise - add reference
        // Promise keeps its reference, future adds its own
        return unique_future<T>(state_, false);  // Calls add_ref() via intrusive_ptr
    }

    inline unique_future<void> promise<void>::get_future() noexcept {
        assert(state_ && "get_future() on moved-from promise<void>");
        // Future shares state with promise - add reference
        return unique_future<void>(state_, false);  // Calls add_ref() via intrusive_ptr
    }

}

#include <actor-zeta/detail/coroutine.hpp>

namespace actor_zeta {

    // ============================================================================
    // Awaiter with continuation support (used by promise::await_transform)
    // ============================================================================
    //
    // NOTE: This is the ONLY awaiter used in this library. The co_await operator
    // is ONLY available inside actor coroutines via await_transform().
    // External co_await is intentionally NOT supported to enforce thread affinity.
    // ============================================================================
    template<typename T>
    struct awaiter {
        unique_future<T> future_;
        detail::future_state_base* promise_state_;  // Promise state for continuation

        explicit awaiter(unique_future<T>&& f, detail::future_state_base* prom_state = nullptr) noexcept
            : future_(std::move(f))
            , promise_state_(prom_state) {}

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

            // Store coroutine handle in awaited future's state
            // User code will call state->resume_coroutine() when future is ready
            state->set_coroutine(handle);

            // Register awaited future in promise_state for user polling
            // User code can access via: method_future.get_state()->get_awaiting_on()
            if (promise_state_) {
                promise_state_->set_awaiting_on(state);
            }

            // Re-check after set_coroutine() to close race window
            if (future_.is_ready()) {
                return false;  // Don't suspend - another thread set result
            }

            return true;
        }

        T await_resume() {
            return std::move(future_).get();
        }
    };

}