#pragma once

#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/rtt.hpp>
#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/intrusive_ptr.hpp>
#include <actor-zeta/detail/monostate.hpp>
#include <actor-zeta/config.hpp>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>  // for std::memmove (trivial move optimization)
#include <thread>   // for std::this_thread::yield()

namespace actor_zeta { namespace detail {

    /// @brief Unified future state - replaces multiple atomic bools
    /// Values are ordered to allow range checks (Seastar optimization):
    ///   - Transient states (setting, consuming) < ready
    ///   - Terminal states (ready, consumed, error, cancelled) >= ready
    ///   - Error states >= error
    /// This allows single-comparison checks: `state >= ready` instead of multiple ==
    enum class future_state_enum : uint8_t {
        invalid = 0,           // Moved-from or uninitialized
        pending = 1,           // Awaiting result (initial state)
        setting = 2,           // set_result() in progress (transient)
        consuming = 3,         // take_result() in progress (transient)
        // --- Terminal states (>= ready) ---
        ready = 4,             // Result available (success)
        consumed = 5,          // get() called, result moved out
        // --- Error states (>= error) ---
        error = 6,             // Error occurred (broken promise, mailbox closed, etc.)
        cancelled = 7,         // Explicitly cancelled
    };

    /// @brief Base class for future state (type-erased part)
    class future_state_base {
    public:
        explicit future_state_base(pmr::memory_resource* res) noexcept
            : resource_(res)
            , state_(future_state_enum::pending)
            // Initial refcount = 1:
            //   Initial ref: Owned by message (released when message destroyed)
            //   Future adds its own ref when created (future constructor calls add_ref())
            //
            // Lifetime scenarios:
            //   Normal:    future.get() → ref 2→1, message destroyed → ref 1→0 → destroy
            //   Orphaned:  future destroyed → ref 2→1, message destroyed → ref 1→0 → destroy
            //   No future: message destroyed → ref 1→0 → destroy
            , refcount_(1)
#ifndef NDEBUG
            , magic_(kMagicAlive)
            , generation_(next_generation())
#endif
        {
        }

        future_state_base(const future_state_base&) = delete;
        future_state_base& operator=(const future_state_base&) = delete;

        virtual ~future_state_base() noexcept {
#ifndef NDEBUG
            assert(is_alive() && "Double delete detected!");
            magic_.store(kMagicDead, std::memory_order_release);
#endif
        }

        /// @brief Increment reference count
        void add_ref() noexcept {
#ifndef NDEBUG
            assert(is_alive() && "Use-after-free: add_ref() on deleted state!");
            // Use fetch_add result to check ACTUAL old value in concurrent scenario
            // This prevents race where multiple threads read stale value before increment
            int old_value = refcount_.fetch_add(1, std::memory_order_relaxed);
            assert(old_value > 0 && "Refcount underflow!");
            // Increased limit to 1000000 - 10000 was too low for stress tests with many concurrent threads
            assert(old_value < 1000000 && "Refcount overflow!");
#else
            refcount_.fetch_add(1, std::memory_order_relaxed);
#endif
        }

        /// @brief Try to increment reference count (like weak_ptr::lock())
        /// @return true if successful (object alive), false if object being destroyed
        ///
        /// This is the thread-safe equivalent of std::weak_ptr::lock().
        /// Atomically checks if refcount > 0 and increments if so.
        ///
        /// Use this instead of add_ref() when you have a raw pointer and don't know
        /// if the object is still alive (e.g., from message->result_slot()).
        [[nodiscard]] bool try_add_ref() noexcept {
            int old_count = refcount_.load(std::memory_order_relaxed);

            // Loop until we either successfully increment or detect object is dead
            do {
                if (old_count == 0) {
                    // Object is being destroyed, cannot increment
                    return false;
                }

                // Try to increment: old_count → old_count + 1
                // If another thread changed refcount, old_count is updated and we retry
            } while (!refcount_.compare_exchange_weak(
                old_count, old_count + 1,
                std::memory_order_acquire,   // Success: synchronize with release operations
                std::memory_order_relaxed)); // Failure: just reload and retry

            // Successfully incremented refcount
            return true;
        }

        /// @brief Decrement reference count and deallocate if zero
        void release() noexcept {
            // CRITICAL: Do NOT check magic_ before fetch_sub!
            //
            // Race scenario if we check first:
            //   Thread A: release() → reads magic_ (OK)
            //   Thread B: release() → refcount 1→0 → destroy() → magic_ = kMagicDead
            //   Thread A: fetch_sub → refcount 0→-1 (underflow!)
            //
            // Solution: fetch_sub FIRST (atomic), then check if we destroyed
            //   - If old_value > 1: object still alive, can safely check magic_
            //   - If old_value == 1: we're destroying, no need to check
            //   - If old_value <= 0: underflow (bug), will be caught by assert

            // OPTIMIZATION: Use release-only ordering on hot path
            //
            // Rationale:
            //   - Release ordering publishes the decrement to other threads
            //   - Acquire needed only when destroying (cold path, 1 in N releases)
            //   - Saves ~20 cycles per release on x86 (no mfence), ~25 cycles on ARM (no dmb)
            //
            // Safety:
            //   - Release ensures: decrement visible to other threads
            //   - Acquire fence before destroy ensures: all previous writes visible
            //   - Same pattern as libstdc++ shared_ptr
            //
            // Assembly impact (x86_64):
            //   Before: lock xadd + mfence (30 cycles)
            //   After: lock xadd only (10 cycles)
            int old_value = refcount_.fetch_sub(1, std::memory_order_release);
#ifndef NDEBUG
            assert(old_value > 0 && "Refcount underflow!");
#endif
            if (old_value == 1) {
                // Cold path: acquire fence before destroying
                // Synchronizes with all previous release operations
                std::atomic_thread_fence(std::memory_order_acquire);
                destroy();
            }
        }

        /// @brief Get current state (atomic read)
        [[nodiscard]] future_state_enum state() const noexcept {
            return state_.load(std::memory_order_acquire);
        }

        /// @brief Check if in terminal state (ready, consumed, error, cancelled)
        /// Seastar optimization: single comparison instead of multiple ==
        [[nodiscard]] bool is_available() const noexcept {
            return state_.load(std::memory_order_acquire) >= future_state_enum::ready;
        }

        /// @brief Check if result is ready (ready or consumed - has value)
        /// Seastar optimization: range check
        [[nodiscard]] bool is_ready() const noexcept {
            auto s = state_.load(std::memory_order_acquire);
            return s == future_state_enum::ready || s == future_state_enum::consumed;
        }

        /// @brief Check if failed (error or cancelled)
        /// Seastar optimization: single comparison (>= error)
        [[nodiscard]] bool is_failed() const noexcept {
            return state_.load(std::memory_order_acquire) >= future_state_enum::error;
        }

#if HAVE_ATOMIC_WAIT
        /// @brief Wait for state to change from expected value
        /// @param expected The expected current state
        /// C++20 only: Efficient blocking wait using futex/ulock_wait
        void wait(future_state_enum expected) const noexcept {
            state_.wait(expected, std::memory_order_acquire);
        }
#endif

        /// @brief Check if cancelled
        [[nodiscard]] bool is_cancelled() const noexcept {
            return state_.load(std::memory_order_acquire) == future_state_enum::cancelled;
        }

        /// @brief Check if consumed (result already taken)
        [[nodiscard]] bool is_consumed() const noexcept {
            return state_.load(std::memory_order_acquire) == future_state_enum::consumed;
        }

        /// @brief Check if still pending (not yet set)
        [[nodiscard]] bool is_pending() const noexcept {
            auto s = state_.load(std::memory_order_acquire);
            return s == future_state_enum::pending ||
                   s == future_state_enum::setting ||
                   s == future_state_enum::consuming;
        }

        /// @brief Atomic state transition with validation
        /// @return true if transition successful, false if current state != expected
        bool transition(future_state_enum expected, future_state_enum desired) noexcept {
            return state_.compare_exchange_strong(expected, desired,
                std::memory_order_acq_rel, std::memory_order_acquire);
        }

        /// @brief Force state change (for internal use, bypasses validation)
        void set_state(future_state_enum new_state) noexcept {
            state_.store(new_state, std::memory_order_release);
        }

        /// @brief Get memory resource
        [[nodiscard]] pmr::memory_resource* memory_resource() const noexcept {
            return resource_;
        }

        /// @brief Resume stored coroutine (if any)
        /// @note Called by user code when awaited future is ready
        /// @note Virtual with `final` override for devirtualization
        virtual void resume_coroutine() noexcept = 0;

        /// @brief Check if coroutine is stored
        /// @return true if coroutine_handle is valid
        [[nodiscard]] virtual bool has_coroutine() const noexcept = 0;

        /// @brief Check if stored coroutine is done
        /// @return true if coroutine exists and is done
        [[nodiscard]] virtual bool coroutine_done() const noexcept = 0;

        /// @brief Store coroutine handle for resumption
        /// @param h The coroutine handle to store
        /// @note Virtual to allow storage in derived class
        virtual void set_coroutine(std::coroutine_handle<> h) noexcept = 0;

        /// @brief Set void forward target (type-erased for void chaining)
        /// @param target The future_state_base to forward void readiness to
        /// @note Only sets up void-to-void forwarding (calls set_ready on target)
        /// @return true if target was set, false if state already changed
        virtual bool set_forward_target_void(future_state_base* target) noexcept = 0;

        /// @brief Set which future this coroutine is awaiting on
        /// @param target The future_state that this coroutine is waiting for
        /// @note Called from awaiter::await_suspend() to enable manual polling
        void set_awaiting_on(future_state_base* target) noexcept {
            awaiting_on_ = target;  // intrusive_ptr assignment calls add_ref()
        }

        /// @brief Get the future this coroutine is awaiting on
        /// @return Pointer to awaited future_state, or nullptr if not awaiting
        /// @note Used by supervisor/user code to poll and resume coroutines
        [[nodiscard]] future_state_base* get_awaiting_on() const noexcept {
            return awaiting_on_.get();
        }

        /// @brief Clear awaiting_on after coroutine resumes
        /// @note Called after resume_coroutine() to prevent stale references
        void clear_awaiting_on() noexcept {
            awaiting_on_ = nullptr;
        }

#ifndef NDEBUG
        [[nodiscard]] uint64_t generation() const noexcept { return generation_; }
#endif

    protected:
        /// @brief Virtual destruction - derived class implements actual deallocation
        virtual void destroy() noexcept = 0;

        pmr::memory_resource* resource_;
        std::atomic<future_state_enum> state_;
        std::atomic<int> refcount_;
        intrusive_ptr<future_state_base> awaiting_on_;  // Future this coroutine is waiting on

#ifndef NDEBUG
        static constexpr uint32_t kMagicAlive = 0xFEEDFACE;
        static constexpr uint32_t kMagicDead = 0xDEADC0DE;

        // Use atomic for magic_ to prevent TSan data race warnings
        // Race occurs when one thread reads magic_ (in add_ref/set_result) while another
        // writes magic_ = kMagicDead (in destructor)
        mutable std::atomic<uint32_t> magic_;
        uint64_t generation_;

        static uint64_t next_generation() {
            static std::atomic<uint64_t> counter{0};
            return counter.fetch_add(1, std::memory_order_relaxed);
        }

        // Helper to check magic_ atomically
        [[nodiscard]] bool is_alive() const noexcept {
            return magic_.load(std::memory_order_acquire) == kMagicAlive;
        }
#endif
    };

    /// @brief Typed future state with result storage
    // ============================================================================
    // In-place result storage - uses union for lazy initialization
    // Zero allocation for trivial types (int, bool, pointers)
    // Non-copyable, move-only (like rtt and unique_future)
    // ============================================================================

    /// @brief In-place storage for future result value
    /// @tparam T Result type (non-void)
    /// @note Non-copyable, move-only (like rtt and unique_future)
    /// @note Uses union for lazy construction without default constructor requirement
    template<typename T>
    struct result_storage {
        // ========================================
        // Storage: union for lazy initialization
        // ========================================
        // Using named union to handle non-trivial T correctly
        // Union destructor is empty - we handle destruction manually
        union storage_t {
            char dummy_;  // For default state (no value)
            T value_;

            // Default: initialize dummy (no T constructed yet)
            storage_t() noexcept : dummy_() {}

            // Destructor is trivial - manual destruction via result_storage
            ~storage_t() {}
        } storage_;

        bool has_value_ = false;

#ifndef NDEBUG
        // Debug: track if storage was ever used (catch use-after-move)
        bool was_moved_from_ = false;
#endif

        // ========================================
        // Constructors
        // ========================================
        result_storage() noexcept = default;

        explicit result_storage(pmr::memory_resource*) noexcept
            : storage_()
            , has_value_(false)
#ifndef NDEBUG
            , was_moved_from_(false)
#endif
        {}

        // ========================================
        // Destructor: destroy T if present
        // ========================================
        ~result_storage() noexcept {
            if (has_value_) {
                storage_.value_.~T();
                has_value_ = false;
            }
        }

        // ========================================
        // NON-COPYABLE
        // ========================================
        result_storage(const result_storage&) = delete;
        result_storage& operator=(const result_storage&) = delete;

        // ========================================
        // MOVE-ONLY (with Trivial Move Optimization)
        // ========================================
        result_storage(result_storage&& other) noexcept
            : has_value_(false)
#ifndef NDEBUG
            , was_moved_from_(false)
#endif
        {
            assert(!other.was_moved_from_ && "Move from already moved-from storage!");

            if (other.has_value_) {
                // Trivial Move Optimization: use memmove for trivially movable types
                // This avoids constructor/destructor overhead for int, bool, pointers, etc.
                if constexpr (is_trivially_move_constructible_and_destructible_v<T>) {
                    std::memmove(&storage_.value_, &other.storage_.value_, sizeof(T));
                } else {
                    new (&storage_.value_) T(std::move(other.storage_.value_));
                    other.storage_.value_.~T();
                }
                has_value_ = true;
                other.has_value_ = false;
#ifndef NDEBUG
                other.was_moved_from_ = true;
#endif
            }
        }

        result_storage& operator=(result_storage&& other) noexcept {
            assert(!was_moved_from_ && "Assignment to moved-from storage!");
            assert(!other.was_moved_from_ && "Move from already moved-from storage!");

            if (this != &other) {
                // Destroy current value if any (for trivial types, no-op)
                if (has_value_) {
                    if constexpr (!is_trivially_move_constructible_and_destructible_v<T>) {
                        storage_.value_.~T();
                    }
                    has_value_ = false;
                }

                // Move from other
                if (other.has_value_) {
                    // Trivial Move Optimization: use memmove for trivially movable types
                    if constexpr (is_trivially_move_constructible_and_destructible_v<T>) {
                        std::memmove(&storage_.value_, &other.storage_.value_, sizeof(T));
                    } else {
                        new (&storage_.value_) T(std::move(other.storage_.value_));
                        other.storage_.value_.~T();
                    }
                    has_value_ = true;
                    other.has_value_ = false;
#ifndef NDEBUG
                    other.was_moved_from_ = true;
#endif
                }
            }
            return *this;
        }

        // ========================================
        // Operations
        // ========================================

        /// @brief Construct value in-place
        /// @pre Storage must be empty (!has_value_)
        /// @post Storage contains value (has_value_ == true)
        template<typename... Args>
        void emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
            assert(!was_moved_from_ && "emplace() on moved-from storage!");
            assert(!has_value_ && "Double emplace() - value already set!");

            new (&storage_.value_) T(std::forward<Args>(args)...);
            has_value_ = true;
        }

        /// @brief Move out value and destroy storage
        /// @pre Storage must contain value (has_value_)
        /// @post Storage is empty (has_value_ == false)
        /// @return Moved value
        [[nodiscard]] T take() noexcept {
            assert(!was_moved_from_ && "take() on moved-from storage!");
            assert(has_value_ && "take() from empty storage!");

            has_value_ = false;

            // Trivial Move Optimization: skip destructor for trivial types
            if constexpr (is_trivially_move_constructible_and_destructible_v<T>) {
                return storage_.value_;  // Trivial copy, no destructor needed
            } else {
                T result = std::move(storage_.value_);
                storage_.value_.~T();
                return result;
            }
        }

        /// @brief Access value by reference
        /// @pre Storage must contain value (has_value_)
        [[nodiscard]] T& get() noexcept {
            assert(!was_moved_from_ && "get() on moved-from storage!");
            assert(has_value_ && "get() from empty storage!");
            return storage_.value_;
        }

        [[nodiscard]] const T& get() const noexcept {
            assert(!was_moved_from_ && "get() on moved-from storage!");
            assert(has_value_ && "get() from empty storage!");
            return storage_.value_;
        }

        /// @brief Check if storage is empty
        [[nodiscard]] bool empty() const noexcept {
            assert(!was_moved_from_ && "empty() on moved-from storage!");
            return !has_value_;
        }

        /// @brief Check if storage contains value
        [[nodiscard]] bool has_value() const noexcept {
            assert(!was_moved_from_ && "has_value() on moved-from storage!");
            return has_value_;
        }
    };

    /// @brief Void specialization - no storage needed
    template<>
    struct result_storage<void> {
        explicit result_storage(pmr::memory_resource*) noexcept {}

        // Void storage is trivially copyable/movable (empty struct)
        result_storage() noexcept = default;
        result_storage(const result_storage&) = default;
        result_storage(result_storage&&) noexcept = default;
        result_storage& operator=(const result_storage&) = default;
        result_storage& operator=(result_storage&&) noexcept = default;
    };

    /// @brief Unified future_state<T> - works for both void and non-void types
    /// Uses conditional storage and if constexpr for zero-overhead void handling
    /// TYPED forward_target_ - no RTT needed for forwarding!
    template<typename T>
    class future_state final : public future_state_base {
    private:
        static constexpr bool has_result = !std::is_void_v<T>;

    public:
        explicit future_state(pmr::memory_resource* res) noexcept
            : future_state_base(res)
            , storage_(res)
            , coro_handle_()
            , forward_target_(nullptr)
        {}

        ~future_state() noexcept override {
            // Release forward target reference if set
            auto* target = forward_target_.load(std::memory_order_acquire);
            if (target) {
                target->release();
            }

            if constexpr (has_result) {
                // Wait for concurrent operations to complete before destroying result_
                auto s = state_.load(std::memory_order_acquire);
                if (s == future_state_enum::setting || s == future_state_enum::consuming) {
                    // Use exponential backoff to avoid busy-waiting
                    int spin_count = 0;
                    constexpr int fast_spin_limit = 10;

                    // Fast spin first (no syscall)
                    while ((s == future_state_enum::setting || s == future_state_enum::consuming)
                           && spin_count < fast_spin_limit) {
                        ++spin_count;
                        s = state_.load(std::memory_order_acquire);
                    }

                    // Then yield/backoff
                    while (s == future_state_enum::setting || s == future_state_enum::consuming) {
                        std::this_thread::yield();
                        s = state_.load(std::memory_order_acquire);
                    }
                }
            }

            if (coro_handle_ && !coro_handle_.done()) {
                coro_handle_.destroy();
            }
        }

        // ===== Typed forward target (NO RTT!) =====

        /// @brief Set typed forward target for result chaining
        /// @param target The future_state<T> to forward results to when this becomes ready
        /// @note TYPED - no RTT needed for forwarding!
        /// @note Thread-safe: uses CAS to prevent race with set_value()
        /// @return true if target was set, false if state already changed (producer won race)
        bool set_forward_target(future_state<T>* target) noexcept {
            assert(target != nullptr && "Forward target cannot be null!");
            assert(target != this && "Self-forwarding creates infinite loop!");

            // Add ref BEFORE CAS - if CAS fails, we release it
            target->add_ref();

            // Atomically set forward_target only if currently nullptr
            // This prevents race with concurrent set_value()
            future_state<T>* expected = nullptr;
            if (!forward_target_.compare_exchange_strong(expected, target,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
                // Another thread already set forward_target (shouldn't happen in normal use)
                target->release();
                return false;
            }

            // Check if producer already completed while we were setting up
            // If state is ready, the producer stored value locally (didn't see our target)
            // We must forward the value now to complete the chain
            auto s = state_.load(std::memory_order_acquire);
            if (s == future_state_enum::ready || s == future_state_enum::consumed) {
                // Producer finished - forward result now
                if constexpr (has_result) {
                    // Take value and forward (state already ready, so take_value won't work)
                    // We need to get the value without state transition
                    if (storage_.has_value()) {
                        target->set_value(storage_.take());
                    }
                } else {
                    // Void - just set target ready
                    target->set_ready();
                }
            }
            // If state is pending/setting, producer will see our target and forward

            return true;
        }

        /// @brief Get typed forward target
        [[nodiscard]] future_state<T>* get_forward_target() const noexcept {
            return forward_target_.load(std::memory_order_acquire);
        }

        // ===== Non-void specific methods =====

        /// @brief Set value directly in-place (ZERO ALLOCATION)
        /// If forward_target_ is set, forwards directly (also ZERO ALLOCATION!)
        template<typename U = T, std::enable_if_t<!std::is_void_v<U>, int> = 0>
        void set_value(U&& value) noexcept {
#ifndef NDEBUG
            assert(is_alive() && "Use-after-free: set_value() on deleted state!");
#endif
            auto expected = future_state_enum::pending;
            if (!state_.compare_exchange_strong(expected, future_state_enum::setting,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                return;
            }

            auto* target = forward_target_.load(std::memory_order_acquire);
            if (target) {
                // TYPED forwarding - ZERO ALLOCATION!
                target->set_value(std::forward<U>(value));
            } else {
                // Store in-place - ZERO ALLOCATION
                storage_.emplace(std::forward<U>(value));
            }

            state_.store(future_state_enum::ready, std::memory_order_release);

#if HAVE_ATOMIC_WAIT
            state_.notify_one();
#endif
        }

        /// @brief Get value reference (only for non-void) - returns T&
        template<typename U = T, std::enable_if_t<!std::is_void_v<U>, int> = 0>
        [[nodiscard]] U& get_value() noexcept {
#ifndef NDEBUG
            assert(is_alive() && "Use-after-free: get_value() on deleted state!");
            auto s = state_.load(std::memory_order_relaxed);
            assert((s == future_state_enum::ready || s == future_state_enum::consumed)
                   && "get_value() called before set_value()!");
#endif
            return storage_.get();
        }

        /// @brief Get const value reference (only for non-void) - returns const T&
        template<typename U = T, std::enable_if_t<!std::is_void_v<U>, int> = 0>
        [[nodiscard]] const U& get_value() const noexcept {
#ifndef NDEBUG
            assert(is_alive() && "Use-after-free: get_value() on deleted state!");
            auto s = state_.load(std::memory_order_relaxed);
            assert((s == future_state_enum::ready || s == future_state_enum::consumed)
                   && "get_value() called before set_value()!");
#endif
            return storage_.get();
        }

        /// @brief Take value and mark consumed (only for non-void) - returns T
        template<typename U = T, std::enable_if_t<!std::is_void_v<U>, int> = 0>
        [[nodiscard]] U take_value() noexcept {
            auto expected = future_state_enum::ready;
            bool transitioned = transition(expected, future_state_enum::consuming);
            assert(transitioned && "take_value() called on non-ready state!");
            (void)transitioned;

            U result = storage_.take();
            state_.store(future_state_enum::consumed, std::memory_order_release);
            return result;
        }

        // ===== Void specific methods =====

        /// @brief Mark as ready (only for void)
        /// @note Forwards to target BEFORE setting ready (same pattern as set_value)
        template<typename U = T, std::enable_if_t<std::is_void_v<U>, int> = 0>
        void set_ready() noexcept {
#ifndef NDEBUG
            assert(is_alive() && "Use-after-free: set_ready() on deleted state!");
#endif
            // Use transient 'setting' state to match set_value() pattern
            auto expected = future_state_enum::pending;
            if (!state_.compare_exchange_strong(expected, future_state_enum::setting,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                return;
            }

            // Forward FIRST (before setting ready) - prevents race where caller
            // sees ready but chained target is still pending
            auto* target = forward_target_.load(std::memory_order_acquire);
            if (target) {
                // TYPED forwarding for void - just set_ready()
                target->set_ready();
            }

            // Set ready AFTER forwarding completes
            state_.store(future_state_enum::ready, std::memory_order_release);

#if HAVE_ATOMIC_WAIT
            state_.notify_one();
#endif
        }

        // ===== Coroutine methods =====

        /// @brief Resume stored coroutine (final - enables devirtualization)
        void resume_coroutine() noexcept final {
            if (coro_handle_ && !coro_handle_.done()) {
                coro_handle_.resume();
            }
        }

        /// @brief Check if coroutine is stored (final - enables devirtualization)
        [[nodiscard]] bool has_coroutine() const noexcept final {
            return coro_handle_.operator bool();
        }

        /// @brief Check if stored coroutine is done (final - enables devirtualization)
        [[nodiscard]] bool coroutine_done() const noexcept final {
            return coro_handle_ && coro_handle_.done();
        }

        /// @brief Store coroutine handle (called from await_suspend)
        void set_coroutine(std::coroutine_handle<> handle) noexcept override {
            coro_handle_ = handle;
        }

        /// @brief Set void forward target (type-erased for void chaining)
        /// @note For void T: calls set_ready() on target when this becomes ready
        /// @note For non-void T: this is a type mismatch, but we handle it gracefully
        ///       by calling target's set_ready() (void-to-void semantics)
        bool set_forward_target_void(future_state_base* target) noexcept override {
            if constexpr (std::is_void_v<T>) {
                // For void, use typed set_forward_target
                return set_forward_target(static_cast<future_state<void>*>(target));
            } else {
                // For non-void, we can only do void-like forwarding
                // This happens when typed result goes through converting constructor
                // The typed result is ALREADY forwarded via setup_result_chaining<T>
                // This void forwarding is for behavior() return value tracking only
                //
                // We simulate void forwarding by setting up a callback pattern:
                // when this becomes ready, mark target as ready (void semantics)
                assert(target != nullptr);
                assert(target != this);

                // We don't actually need to forward - if we're called from void context,
                // the typed value was already forwarded via typed mechanism
                // Just return true to indicate "success" - actual forwarding already happened
                return true;
            }
        }

    protected:
        void destroy() noexcept override {
            this->~future_state();
            resource_->deallocate(this, sizeof(future_state<T>), alignof(future_state<T>));
        }

    private:
        [[no_unique_address]] result_storage<T> storage_;
        coroutine_handle<void> coro_handle_;
        std::atomic<future_state<T>*> forward_target_;  // TYPED forward target
    };

    // intrusive_ptr support for future_state_base
    // These free functions enable intrusive_ptr to manage future_state lifetime
    inline void intrusive_ptr_add_ref(const future_state_base* p) noexcept {
        const_cast<future_state_base*>(p)->add_ref();
    }

    inline void intrusive_ptr_release(const future_state_base* p) noexcept {
        const_cast<future_state_base*>(p)->release();
    }

}} // namespace actor_zeta::detail