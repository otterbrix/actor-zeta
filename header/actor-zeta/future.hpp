#pragma once

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
    // Forward declaration for operator co_await
    template<typename T>
    struct future_awaiter;
#endif

    /// @brief Promise - write interface for async operations
    template<typename T>
    class promise final {
    public:
        promise() = delete;
        promise(const promise&) = delete;
        promise& operator=(const promise&) = delete;

        /// @brief Construct promise from future_state pointer
        explicit promise(detail::future_state_base* slot, pmr::memory_resource* res) noexcept
            : slot_(slot)
            , resource_(res) {
            assert(slot_ && "promise constructed with null slot");
        }

        /// @brief Move constructor
        promise(promise&& other) noexcept
            : slot_(other.slot_)
            , resource_(other.resource_) {
            other.slot_ = nullptr;
        }

        /// @brief Move assignment
        promise& operator=(promise&& other) noexcept {
            if (this != &other) {
                slot_ = other.slot_;
                resource_ = other.resource_;
                other.slot_ = nullptr;
            }
            return *this;
        }

        ~promise() noexcept = default;

        /// @brief Set successful result
        void set_value(T&& value) {
            assert(slot_ && "set_value() on moved-from promise");
            slot_->set_result_rtt(detail::rtt(resource_, std::forward<T>(value)));
        }

        void set_value(const T& value) {
            assert(slot_ && "set_value() on moved-from promise");
            slot_->set_result_rtt(detail::rtt(resource_, value));
        }

        /// @brief Check if promise is valid
        [[nodiscard]] bool is_valid() const noexcept {
            return slot_ != nullptr;
        }

        /// @brief Get slot pointer (does NOT transfer ownership)
        [[nodiscard]] detail::future_state_base* slot() const noexcept {
            return slot_;
        }

    private:
        detail::future_state_base* slot_;
        pmr::memory_resource* resource_;
    };

    /// @brief Promise specialization for void
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
            slot_->set_result_rtt(detail::rtt(resource_, int{0}));
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

    /// @brief Future for async results
    template<typename T>
    class unique_future final {
#if HAVE_STD_COROUTINES
        template<typename U>
        friend struct future_awaiter;  // Allow awaiter to access state_
#endif

    public:
        /// @brief Default constructor - creates invalid future
        explicit unique_future(pmr::memory_resource* /*res*/) noexcept
            : mode_(storage_mode::invalid)
            , needs_scheduling_(false) {
            storage_.state_ = nullptr;
        }

        unique_future(const unique_future&) = delete;
        unique_future& operator=(const unique_future&) = delete;

        /// @brief Construct from future_state pointer (STATE mode)
        /// @param state Future state pointer (ownership transferred)
        /// @param needs_sched true if actor was unblocked (needs scheduling)
        explicit unique_future(detail::future_state<T>* state, bool needs_sched = false) noexcept
            : mode_(storage_mode::state)
            , needs_scheduling_(needs_sched) {
            storage_.state_ = state;
        }

        /// @brief Implicit conversion constructor (IMMEDIATE mode)
        /// @param value Immediate value to store
        /// @note This enables: unique_future<int> f = 42;
        unique_future(T&& value) noexcept(std::is_nothrow_move_constructible<T>::value)
            : mode_(storage_mode::immediate)
            , needs_scheduling_(false) {
            new (&storage_.value_) T(std::forward<T>(value));
        }

        /// @brief Implicit conversion from const lvalue
        unique_future(const T& value) noexcept(std::is_nothrow_copy_constructible<T>::value)
            : mode_(storage_mode::immediate)
            , needs_scheduling_(false) {
            new (&storage_.value_) T(value);
        }

        /// @brief Move constructor
        unique_future(unique_future&& other) noexcept
            : mode_(other.mode_)
            , needs_scheduling_(other.needs_scheduling_) {
            // Move storage based on mode
            if (mode_ == storage_mode::state) {
                storage_.state_ = other.storage_.state_;
                other.storage_.state_ = nullptr;
            } else if (mode_ == storage_mode::immediate) {
                new (&storage_.value_) T(std::move(other.storage_.value_));
                other.storage_.value_.~T();
            }

            other.mode_ = storage_mode::invalid;
            other.needs_scheduling_ = false;
        }

        /// @brief Move assignment
        unique_future& operator=(unique_future&& other) noexcept {
            if (this != &other) {
                // Clean up current storage
                if (mode_ == storage_mode::state && storage_.state_) {
                    // RAII cancellation before releasing
                    if (!storage_.state_->is_ready()) {
                        storage_.state_->set_state(detail::future_state_enum::cancelled);
                    }
                    storage_.state_->release();
                } else if (mode_ == storage_mode::immediate) {
                    storage_.value_.~T();
                }

                // Transfer ownership from other
                mode_ = other.mode_;
                needs_scheduling_ = other.needs_scheduling_;

                if (mode_ == storage_mode::state) {
                    storage_.state_ = other.storage_.state_;
                    other.storage_.state_ = nullptr;
                } else if (mode_ == storage_mode::immediate) {
                    new (&storage_.value_) T(std::move(other.storage_.value_));
                    other.storage_.value_.~T();
                }

                other.mode_ = storage_mode::invalid;
                other.needs_scheduling_ = false;
            }
            return *this;
        }

        /// @brief Destructor - handle both modes
        ~unique_future() noexcept {
            if (mode_ == storage_mode::state && storage_.state_) {
                storage_.state_->release();
            } else if (mode_ == storage_mode::immediate) {
                storage_.value_.~T();
            }
        }

        /// @brief Blocking get - wait for result with hybrid waiting strategy
        T get() && {
            // IMMEDIATE mode: return value directly
            if (mode_ == storage_mode::immediate) {
                T result = std::move(storage_.value_);
                storage_.value_.~T();
                mode_ = storage_mode::invalid;
                return result;
            }

            // STATE mode: wait for result
            assert(mode_ == storage_mode::state && "get() on invalid future");
            assert(storage_.state_ && "get() with null state");

            // Hybrid waiting strategy for optimal performance:
            // 1. Fast spin (0-10): For quick operations (0 CPU overhead)
            // 2. Yield loop (11-100): Medium latency operations
            // 3. Exponential backoff (>100): Slow operations (reduce CPU usage)

            int spin_count = 0;
            constexpr int fast_spin_limit = 10;
            constexpr int yield_limit = 100;

            // Phase 1: Fast spin (no yield, no sleep)
            while (!storage_.state_->is_ready() && spin_count < fast_spin_limit) {
                if (storage_.state_->is_cancelled()) {
                    storage_.state_->release();
                    storage_.state_ = nullptr;
                    mode_ = storage_mode::invalid;
                    assert(false && "get() on cancelled future!");
                    return T{};
                }
                ++spin_count;
            }

            // Phase 2: Yield loop
            while (!storage_.state_->is_ready() && spin_count < yield_limit) {
                if (storage_.state_->is_cancelled()) {
                    storage_.state_->release();
                    storage_.state_ = nullptr;
                    mode_ = storage_mode::invalid;
                    assert(false && "get() on cancelled future!");
                    return T{};
                }
                std::this_thread::yield();
                ++spin_count;
            }

            // Phase 3: Exponential backoff with sleep
            auto backoff = std::chrono::microseconds(1);
            constexpr auto max_backoff = std::chrono::microseconds(100);

            while (!storage_.state_->is_ready()) {
                if (storage_.state_->is_cancelled()) {
                    storage_.state_->release();
                    storage_.state_ = nullptr;
                    mode_ = storage_mode::invalid;
                    assert(false && "get() on cancelled future!");
                    return T{};
                }

                std::this_thread::sleep_for(backoff);
                if (backoff < max_backoff) {
                    backoff *= 2;
                }
            }

            // Compiler barrier to prevent reordering
            // The acquire load in is_ready() already provides memory synchronization
            // This just prevents compiler from reordering result_ access
            std::atomic_signal_fence(std::memory_order_acq_rel);

            // Extract result from rtt
            T result = storage_.state_->result().template get<T>(0);

            // Release state
            storage_.state_->release();
            storage_.state_ = nullptr;
            mode_ = storage_mode::invalid;

            return result;
        }

        /// @brief Lvalue get() DELETED
        T get() & = delete;

        /// @brief Check if ready
        [[nodiscard]] bool is_ready() const noexcept {
            if (mode_ == storage_mode::immediate) {
                return true;  // IMMEDIATE mode is always ready
            } else if (mode_ == storage_mode::state) {
                return storage_.state_ && storage_.state_->is_ready();
            }
            return false;  // invalid mode
        }

        /// @brief Check if valid
        [[nodiscard]] bool valid() const noexcept {
            return mode_ != storage_mode::invalid;
        }

        /// @brief Check if needs scheduling
        [[nodiscard]] bool needs_scheduling() const noexcept {
            return needs_scheduling_;
        }

        /// @brief Request cancellation (STATE mode only)
        void cancel() noexcept {
            if (mode_ == storage_mode::state && storage_.state_) {
                storage_.state_->set_state(detail::future_state_enum::cancelled);
            }
        }

        /// @brief Check if cancelled (STATE mode only)
        [[nodiscard]] bool is_cancelled() const noexcept {
            return mode_ == storage_mode::state && storage_.state_ && storage_.state_->is_cancelled();
        }

        /// @brief Check if in IMMEDIATE mode
        [[nodiscard]] bool is_immediate() const noexcept {
            return mode_ == storage_mode::immediate;
        }

        /// @brief Check if in STATE mode
        [[nodiscard]] bool is_state() const noexcept {
            return mode_ == storage_mode::state;
        }

        /// @brief Extract immediate value (IMMEDIATE mode only, move-only)
        /// @note Used by handler.hpp to convert IMMEDIATE → STATE
        T take_immediate_value() && {
            assert(mode_ == storage_mode::immediate && "take_immediate_value() on non-immediate future");
            T value = std::move(storage_.value_);
            storage_.value_.~T();
            mode_ = storage_mode::invalid;
            return value;
        }

        /// @brief Extract state pointer (STATE mode only, move-only)
        /// @note Used by handler.hpp to link state to message
        detail::future_state<T>* take_state() && {
            assert(mode_ == storage_mode::state && "take_state() on non-state future");
            auto* state = storage_.state_;
            storage_.state_ = nullptr;
            mode_ = storage_mode::invalid;
            return state;
        }

        /// @brief Get memory resource (STATE mode only)
        /// @note Used by operator co_await to get resource for continuation allocation
        [[nodiscard]] pmr::memory_resource* memory_resource() const noexcept {
            if (mode_ == storage_mode::state && storage_.state_) {
                return storage_.state_->memory_resource();
            }
            return pmr::get_default_resource();
        }

#if HAVE_STD_COROUTINES
        // operator co_await moved to free function (like Seastar)

        /// @brief Register continuation callback - PHASE 4 with automatic unwrapping
        /// @param callback Function to call when future becomes ready (signature: R(T) or unique_future<R>(T))
        /// @return New future for callback result (automatically unwrapped if callback returns future)
        /// @note PHASE 4: Enables functional-style continuation chains with automatic unwrapping
        ///
        /// Example:
        /// @code
        /// // Normal callback (returns int)
        /// send(actor, sender, &Actor::compute, 42)
        ///     .then([](int result) {
        ///         return result * 2;  // Returns int
        ///     });  // Returns unique_future<int>
        ///
        /// // Callback returning future (automatic unwrap)
        /// send(actor1, sender, &Actor::step1, 42)
        ///     .then([](int result) {
        ///         return send(actor2, sender, &Actor::step2, result);  // Returns unique_future<int>
        ///     });  // Returns unique_future<int> (NOT unique_future<unique_future<int>>!)
        /// @endcode
        template<typename F>
        auto then(F&& callback) -> unique_future<
            typename std::conditional_t<
                type_traits::is_unique_future_v<typename std::invoke_result<F, T>::type>,
                typename type_traits::is_unique_future<typename std::invoke_result<F, T>::type>::value_type,
                typename std::invoke_result<F, T>::type
            >
        > {
            using R = typename std::invoke_result<F, T>::type;

            // Unwrapped result type: if R is unique_future<U>, then U; else R
            using unwrapped_result_t = typename std::conditional_t<
                type_traits::is_unique_future_v<R>,
                typename type_traits::is_unique_future<R>::value_type,
                R
            >;

            // Get memory resource for allocations
            pmr::memory_resource* resource = nullptr;
            if (mode_ == storage_mode::state && storage_.state_) {
                resource = storage_.state_->memory_resource();
            } else {
                resource = pmr::get_default_resource();
            }

            // IMMEDIATE mode: invoke callback immediately
            if (mode_ == storage_mode::immediate) {
                // Extract value and invoke callback
                T value = std::move(storage_.value_);
                storage_.value_.~T();
                mode_ = storage_mode::invalid;

                if constexpr (std::is_void_v<R>) {
                    // Callback returns void
                    callback(std::move(value));
                    // Return invalid future for void
                    return unique_future<unwrapped_result_t>(resource);
                } else if constexpr (type_traits::is_unique_future_v<R>) {
                    // UNWRAPPING: Callback returns unique_future<U>
                    // Call callback to get inner future, then return it (already unwrapped)
                    R inner_future = callback(std::move(value));
                    return inner_future;  // Returns unique_future<U>, not unique_future<unique_future<U>>
                } else {
                    // Normal case: Callback returns U
                    // Use implicit conversion to wrap in unique_future
                    R result = callback(std::move(value));
                    return unique_future<unwrapped_result_t>(std::move(result));
                }
            }

            // STATE mode: register continuation
            assert(mode_ == storage_mode::state && "then() on invalid future");
            assert(storage_.state_ && "then() with null state");

            // Create new future_state for UNWRAPPED result type
            void* mem = resource->allocate(sizeof(detail::future_state<unwrapped_result_t>),
                                          alignof(detail::future_state<unwrapped_result_t>));
            auto* new_state = new (mem) detail::future_state<unwrapped_result_t>(resource);
            new_state->add_ref();  // Initial reference for returned future

            // Capture current state and new state in continuation
            auto* current_state = storage_.state_;
            current_state->add_ref();  // Keep current state alive for continuation

            // Register continuation
            current_state->add_continuation(detail::unique_function<void()>(resource, [current_state, new_state, callback = std::forward<F>(callback), resource]() mutable {
                // Get result from current future
                assert(current_state->is_ready() && "Continuation called before future ready");

                T value = current_state->result().template get<T>(0);

                if constexpr (std::is_void_v<R>) {
                    // Callback returns void
                    callback(std::move(value));
                    // Mark new future as ready (void has no result)
                    new_state->set_ready();
                } else if constexpr (type_traits::is_unique_future_v<R>) {
                    // UNWRAPPING: Callback returns unique_future<U>
                    // Need to wait for inner future and copy result to new_state

                    // Call callback to get inner future
                    R inner_future = callback(std::move(value));

                    // Register second-level continuation on inner future
                    // This continuation will copy result from inner_future to new_state
                    if (inner_future.is_immediate()) {
                        // Inner future is IMMEDIATE mode - extract value immediately
                        unwrapped_result_t inner_value = std::move(inner_future).take_immediate_value();
                        detail::rtt result_rtt(resource, std::move(inner_value));
                        new_state->set_result(std::move(result_rtt));
                    } else if (inner_future.is_state()) {
                        // Inner future is STATE mode - need to wait for it
                        auto* inner_state = std::move(inner_future).take_state();
                        inner_state->add_ref();  // Keep alive for continuation

                        // Register continuation on inner future
                        inner_state->add_continuation(detail::unique_function<void()>(resource, [inner_state, new_state, resource]() mutable {
                            assert(inner_state->is_ready() && "Inner continuation called before ready");

                            // Copy result from inner future to final future
                            unwrapped_result_t final_value = inner_state->result().template get<unwrapped_result_t>(0);
                            detail::rtt final_rtt(resource, std::move(final_value));
                            new_state->set_result(std::move(final_rtt));

                            // Release inner state
                            inner_state->release();
                        }));

                        // Release our reference (continuation holds one)
                        inner_state->release();
                    } else {
                        // Invalid inner future - mark as cancelled
                        new_state->set_state(detail::future_state_enum::cancelled);
                    }
                } else {
                    // Normal case: Callback returns U
                    R result = callback(std::move(value));
                    // Store result in new future
                    detail::rtt result_rtt(resource, std::move(result));
                    new_state->set_result(std::move(result_rtt));
                }

                // Release reference to current state
                current_state->release();
            }));

            // Move ownership from current future (invalidate this future)
            storage_.state_ = nullptr;
            mode_ = storage_mode::invalid;

            // Return new future with new_state (unwrapped type)
            return unique_future<unwrapped_result_t>(new_state, false);
        }
#endif // HAVE_STD_COROUTINES

#if HAVE_STD_COROUTINES
        /// @brief Promise type for C++20 coroutine support
        /// Enables co_return in actor methods
        ///
        /// Example:
        /// @code
        /// unique_future<int> my_method() {
        ///     co_return 42;  // Uses promise_type::return_value()
        /// }
        /// @endcode
        struct promise_type {
            using value_type = T;

            /// @brief Create future from this promise
            unique_future<T> get_return_object() {

                // Allocate future_state<T>
                void* mem = resource_->allocate(sizeof(detail::future_state<T>),
                                                alignof(detail::future_state<T>));
                state_ = new (mem) detail::future_state<T>(resource_);

                // Store coroutine_handle in state (type erasure: promise_type → void)
                auto handle = detail::coroutine_handle<promise_type>::from_promise(*this);
                state_->set_coroutine(handle);

                // Return future with state ownership
                return unique_future<T>(state_, false);
            }

            /// @brief Initial suspend - start executing immediately (IMMEDIATE mode)
            detail::suspend_never initial_suspend() noexcept {
                return {};
            }

            /// @brief Final suspend - must suspend to prevent coroutine destruction before set_result
            detail::suspend_always final_suspend() noexcept {
                return {};
            }

            /// @brief Handle co_return value
            void return_value(T&& value) noexcept {
                assert(state_ && "return_value() with null state");
                // Store result in future_state
                detail::rtt result(resource_, std::forward<T>(value));
                state_->set_result(std::move(result));
            }

            void return_value(const T& value) noexcept {
                assert(state_ && "return_value() with null state");
                detail::rtt result(resource_, value);
                state_->set_result(std::move(result));
            }

            /// @brief Exception handling - DISABLED (no exceptions in actor-zeta)
            void unhandled_exception() noexcept {
                // actor-zeta compiles with -fno-exceptions
                // This should never be called
                assert(false && "unhandled_exception() should never be called (-fno-exceptions)");
            }

            /// @brief await_transform REMOVED - let compiler call operator co_await directly!

            /// @brief Constructor - SIMPLE VERSION (no SFINAE, just use default resource)
            /// For now, always use default resource to eliminate template complexity
            promise_type() noexcept
                : resource_(pmr::get_default_resource())
                , state_(nullptr) {
            }

            /// @brief Constructor - variadic (ignores all parameters, uses default resource)
            template<typename... Args>
            promise_type(Args&&...) noexcept
                : resource_(pmr::get_default_resource())
                , state_(nullptr) {
            }

            ~promise_type() noexcept = default;

        private:
            pmr::memory_resource* resource_;
            detail::future_state<T>* state_;
        };
#endif // HAVE_STD_COROUTINES

    private:
        /// @brief Storage mode for TWO modes architecture
        enum class storage_mode : uint8_t {
            invalid,    // Invalid/moved-from future
            state,      // MODE 1: Contains future_state (async, from send() or co_return)
            immediate   // MODE 2: Contains immediate value (sync, from implicit conversion)
        };

        /// @brief Union storage for TWO modes
        union storage {
            detail::future_state<T>* state_;  // MODE 1: STATE
            T value_;                          // MODE 2: IMMEDIATE

            storage() noexcept : state_(nullptr) {}
            ~storage() noexcept {}  // No-op, handled by unique_future destructor
        } storage_;

        storage_mode mode_;
        bool needs_scheduling_;
    };

    /// @brief unique_future<void> specialization - read interface for void async operations
    template<>
    class unique_future<void> final {
#if HAVE_STD_COROUTINES
        template<typename U>
        friend struct future_awaiter;  // Allow awaiter to access state_
#endif

    public:
        /// @brief Constructor for invalid future - requires memory_resource
        explicit unique_future(pmr::memory_resource* /*res*/) noexcept
            : state_(nullptr)
            , needs_scheduling_(false) {
        }

        /// @brief Construct from future_state pointer
        /// @param state Future state pointer (ownership transferred)
        /// @param needs_sched true if actor was unblocked (needs scheduling)
        explicit unique_future(detail::future_state<void>* state, bool needs_sched = false) noexcept
            : state_(state)
            , needs_scheduling_(needs_sched) {
        }

        unique_future(const unique_future&) = delete;
        unique_future& operator=(const unique_future&) = delete;

        /// @brief Move constructor
        unique_future(unique_future&& other) noexcept
            : state_(other.state_)
            , needs_scheduling_(other.needs_scheduling_) {
            other.state_ = nullptr;
            other.needs_scheduling_ = false;
        }

        /// @brief Move assignment
        unique_future& operator=(unique_future&& other) noexcept {
            if (this != &other) {
                // RAII cancellation before releasing
                if (state_ && !state_->is_ready()) {
                    state_->set_state(detail::future_state_enum::cancelled);
                }

                // Release current state if any
                if (state_) {
                    state_->release();
                }

                // Transfer ownership
                state_ = other.state_;
                needs_scheduling_ = other.needs_scheduling_;

                other.state_ = nullptr;
                other.needs_scheduling_ = false;
            }
            return *this;
        }

        /// @brief Destructor - RAII cancellation + releases state reference
        ~unique_future() noexcept {
            if (state_) {
                state_->release();
            }
        }

        /// @brief Blocking get - wait for completion with hybrid waiting strategy
        void get() && {
            assert(state_ && "get() on invalid future");

            // Hybrid waiting strategy (same as unique_future<T>)
            int spin_count = 0;
            constexpr int fast_spin_limit = 10;
            constexpr int yield_limit = 100;

            // Phase 1: Fast spin
            while (!state_->is_ready() && spin_count < fast_spin_limit) {
                if (state_->is_cancelled()) {
                    state_->release();
                    state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    return;
                }
                ++spin_count;
            }

            // Phase 2: Yield loop
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

            // Phase 3: Exponential backoff
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

            // Release state
            state_->release();
            state_ = nullptr;
        }

        /// @brief Lvalue get() DELETED - future is consumable
        void get() & = delete;

        /// @brief Non-blocking check if result is ready
        [[nodiscard]] bool is_ready() const noexcept {
            return state_ && state_->is_ready();
        }

        /// @brief Check if future is valid (enqueue succeeded)
        [[nodiscard]] bool valid() const noexcept {
            return state_ != nullptr;
        }

        /// @brief Check if actor needs scheduling
        /// @return true if actor was unblocked (needs scheduler->enqueue()), false otherwise
        [[nodiscard]] bool needs_scheduling() const noexcept {
            return needs_scheduling_;
        }

        /// @brief Request cancellation
        void cancel() noexcept {
            if (state_) {
                state_->set_state(detail::future_state_enum::cancelled);
            }
        }

        /// @brief Check if cancelled
        [[nodiscard]] bool is_cancelled() const noexcept {
            return state_ && state_->is_cancelled();
        }

#if HAVE_STD_COROUTINES
        /// @brief Register continuation callback - PHASE 4 (void specialization)
        /// @param callback Function to call when future becomes ready (signature: R())
        /// @return New future for callback result
        /// @note PHASE 4: Enables functional-style continuation chains for void futures
        template<typename F>
        auto then(F&& callback) -> unique_future<typename std::invoke_result<F>::type> {
            using R = typename std::invoke_result<F>::type;

            // Get memory resource for allocations
            pmr::memory_resource* resource = nullptr;
            if (state_) {
                resource = state_->memory_resource();
            } else {
                resource = pmr::get_default_resource();
            }

            // Check if already invalid
            if (!state_) {
                // Invalid future - return invalid future
                return unique_future<R>(resource);
            }

            // Create new future_state for callback result
            void* mem = resource->allocate(sizeof(detail::future_state<R>), alignof(detail::future_state<R>));
            auto* new_state = new (mem) detail::future_state<R>(resource);
            new_state->add_ref();  // Initial reference for returned future

            // Capture current state and new state in continuation
            auto* current_state = state_;
            current_state->add_ref();  // Keep current state alive for continuation

            // Register continuation
            current_state->add_continuation(detail::unique_function<void()>(resource, [current_state, new_state, callback = std::forward<F>(callback), resource]() mutable {
                // Wait for current future to be ready
                assert(current_state->is_ready() && "Continuation called before future ready");

                if constexpr (std::is_void_v<R>) {
                    // Callback returns void
                    callback();
                    // Mark new future as ready (void has no result)
                    new_state->set_ready();
                } else {
                    // Callback returns value
                    R result = callback();
                    // Store result in new future
                    detail::rtt result_rtt(resource, std::move(result));
                    new_state->set_result(std::move(result_rtt));
                }

                // Release reference to current state
                current_state->release();
            }));

            // Move ownership from current future (invalidate this future)
            state_ = nullptr;

            // Return new future with new_state
            return unique_future<R>(new_state, false);
        }
#endif // HAVE_STD_COROUTINES

#if HAVE_STD_COROUTINES
        /// @brief Promise type for C++20 coroutine support (void specialization)
        /// Enables co_return (without value) in actor methods
        ///
        /// Example:
        /// @code
        /// unique_future<void> my_method() {
        ///     // ... do work ...
        ///     co_return;  // Uses promise_type::return_void()
        /// }
        /// @endcode
        struct promise_type {
            using value_type = void;

            /// @brief Create future from this promise
            unique_future<void> get_return_object() {
                // Allocate future_state<void>
                void* mem = resource_->allocate(sizeof(detail::future_state<void>),
                                                alignof(detail::future_state<void>));
                state_ = new (mem) detail::future_state<void>(resource_);

                // Store coroutine_handle in state (type erasure: promise_type → void)
                auto handle = detail::coroutine_handle<promise_type>::from_promise(*this);
                state_->set_coroutine(handle);

                // Return future with state ownership
                return unique_future<void>(state_, false);
            }

            /// @brief Initial suspend - start executing immediately (IMMEDIATE mode)
            detail::suspend_never initial_suspend() noexcept { return {}; }

            /// @brief Final suspend - must suspend to prevent coroutine destruction before set_ready
            detail::suspend_always final_suspend() noexcept { return {}; }

            /// @brief Handle co_return (without value)
            void return_void() noexcept {
                assert(state_ && "return_void() with null state");
                // Mark as ready (void has no result value)
                state_->set_ready();
            }

            /// @brief Exception handling - DISABLED (no exceptions in actor-zeta)
            void unhandled_exception() noexcept {
                // actor-zeta compiles with -fno-exceptions
                // This should never be called
                assert(false && "unhandled_exception() should never be called (-fno-exceptions)");
            }

            /// @brief Constructor - requires memory resource
            explicit promise_type(pmr::memory_resource* res = pmr::get_default_resource()) noexcept
                : resource_(res)
                , state_(nullptr) {}

        private:
            pmr::memory_resource* resource_;
            detail::future_state<void>* state_;
        };
#endif // HAVE_STD_COROUTINES

    private:
        detail::future_state<void>* state_;  // State ownership (null after move or invalid future)
        bool needs_scheduling_;
    };

} // namespace actor_zeta

// C++20 Coroutine Support - co_await integration
#include <actor-zeta/detail/coroutine.hpp>

#if HAVE_STD_COROUTINES

namespace actor_zeta {

    /// @brief Awaiter for unique_future<T> - enables co_await
    ///
    /// Example:
    /// @code
    /// awaitable<int> my_coroutine() {
    ///     auto future = send(actor, addr, &Actor::compute, 42);
    ///     int result = co_await future;  // Uses this awaiter
    ///     co_return result * 2;
    /// }
    /// @endcode
    template<typename T>
    struct future_awaiter {
        detail::future_state<T>* state_;  // SIMPLIFIED: Store ONLY state pointer!
        pmr::memory_resource* resource_;  // For continuation allocation

        explicit future_awaiter(detail::future_state<T>* s, pmr::memory_resource* res) noexcept
            : state_(s), resource_(res) {
        }

        // TRIVIAL copy/move - just pointer!
        future_awaiter(const future_awaiter&) = default;
        future_awaiter(future_awaiter&&) noexcept = default;
        future_awaiter& operator=(const future_awaiter&) = default;
        future_awaiter& operator=(future_awaiter&&) = default;

        ~future_awaiter() noexcept {
        }

        /// @brief Check if future is ready (fast path - no suspend needed)
        [[nodiscard]] bool await_ready() const noexcept {

            if (!state_) {
                return true;  // Invalid future - don't suspend
            }

            bool ready = state_->is_ready();
            return ready;
        }

        /// @brief Suspend coroutine - setup resumption when future is ready
        /// @return false = resume immediately, true = suspend and wait
        /// @note SIMPLIFIED: Direct state pointer access
        bool await_suspend(std::coroutine_handle<> handle) noexcept {

            if (!state_) {
                return false;  // Invalid - don't suspend
            }

            // Check again after potential race
            if (state_->is_ready()) {
                return false;  // Don't suspend, resume immediately
            }

            // Register continuation that will resume this coroutine

            state_->add_continuation(detail::unique_function<void()>(resource_, [handle]() mutable {
                // Resume coroutine when future becomes ready
                handle.resume();
            }));

            // Suspend coroutine - it will be resumed by the continuation
            return true;
        }

        /// @brief Get result when coroutine resumes
        /// @note SIMPLIFIED: Direct state access
        T await_resume() {

            assert(state_ && "await_resume() with null state");
            assert(state_->is_ready() && "await_resume() called but state not ready!");

            // Extract value from state
            T result = state_->result().template get<T>(0);

            // Release state
            state_->release();
            state_ = nullptr;

            return result;
        }
    };

    /// @brief Awaiter specialization for void
    template<>
    struct future_awaiter<void> {
        unique_future<void>& future_;

        explicit future_awaiter(unique_future<void>& f) noexcept : future_(f) {}

        [[nodiscard]] bool await_ready() const noexcept {
            return future_.is_ready();
        }

        /// @brief Suspend coroutine - setup resumption when future is ready
        /// @return false = resume immediately, true = suspend and wait
        /// @note PHASE 4: Real suspension with continuation callback
        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            // Check again after potential race
            if (future_.is_ready()) {
                return false;  // Don't suspend, resume immediately
            }

            // PHASE 4: Register continuation to resume coroutine when future becomes ready
            // unique_future<void> doesn't use union storage, directly access state_
            auto* state = future_.state_;
            if (!state) {
                // Invalid future - don't suspend
                return false;
            }

            // Register continuation that will resume this coroutine
            // unique_function requires memory resource for construction
            auto* resource = state->memory_resource();
            state->add_continuation(detail::unique_function<void()>(resource, [handle]() mutable {
                // Resume coroutine when future becomes ready
                handle.resume();
            }));

            // Suspend coroutine - it will be resumed by the continuation
            return true;
        }

        void await_resume() {
            assert(future_.valid() && "await_resume() on invalid future");
            std::move(future_).get();
        }
    };

    /// @brief Free function operator co_await - SIMPLIFIED: Extract state pointer!
    /// @note This is a free function, NOT a member function
    template<typename T>
    auto operator co_await(unique_future<T>&& f) noexcept {

        // Handle IMMEDIATE mode - these are always ready
        if (f.is_immediate()) {
            // IMMEDIATE mode can't be awaited - must extract value directly
            // Return awaiter with null state (await_ready will return true)
            return future_awaiter<T>{nullptr, pmr::get_default_resource()};
        }

        // STATE mode: extract state pointer and memory resource using PUBLIC method
        auto* resource = f.memory_resource();

        // Extract state pointer (transfers ownership)
        auto* state = std::move(f).take_state();


        // Return awaiter with state pointer
        return future_awaiter<T>{state, resource};
    }

    /// @brief Lvalue overload - also needed
    template<typename T>
    auto operator co_await(unique_future<T>& f) noexcept {

        // Handle IMMEDIATE mode
        if (f.is_immediate()) {
            return future_awaiter<T>{nullptr, pmr::get_default_resource()};
        }

        // STATE mode: extract state pointer and memory resource using PUBLIC method
        auto* resource = f.memory_resource();

        auto* state = std::move(f).take_state();


        return future_awaiter<T>{state, resource};
    }

} // namespace actor_zeta

#endif // HAVE_STD_COROUTINES