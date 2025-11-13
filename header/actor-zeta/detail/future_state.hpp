#pragma once

#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/rtt.hpp>
#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/unique_function.hpp>
#include <actor-zeta/config.hpp>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

namespace actor_zeta { namespace detail {

    /// @brief Unified future state - replaces multiple atomic bools
    /// Values are ordered to allow range checks (e.g., is_ready = state >= ready)
    enum class future_state_enum : uint8_t {
        invalid = 0,           // Moved-from or uninitialized
        pending = 1,           // Awaiting result (initial state)
        ready = 2,             // Result available (success)
        error = 3,             // Error occurred (broken promise, mailbox closed, etc.)
        consumed = 4,          // get() called, result moved out
        cancelled = 5          // Explicitly cancelled
    };

    /// @brief Base class for future state (type-erased part)
    class future_state_base {
    public:
        explicit future_state_base(pmr::memory_resource* res) noexcept
            : resource_(res)
            , state_(future_state_enum::pending)
            , refcount_(2)  // Initial: 1 for message, 1 for future
#ifndef NDEBUG
            , magic_(kMagicAlive)
            , generation_(next_generation())
#endif
        {}

        future_state_base(const future_state_base&) = delete;
        future_state_base& operator=(const future_state_base&) = delete;

        virtual ~future_state_base() noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Double delete detected!");
            magic_ = kMagicDead;
#endif
        }

        /// @brief Increment reference count
        void add_ref() noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Use-after-free: add_ref() on deleted state!");
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

        /// @brief Decrement reference count and deallocate if zero
        void release() noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Double release detected!");
#endif
            // Use fetch_sub result - it returns the OLD value before decrement
            int old_value = refcount_.fetch_sub(1, std::memory_order_acq_rel);
#ifndef NDEBUG
            assert(old_value > 0 && "Refcount underflow!");
#endif
            if (old_value == 1) {
                destroy();
            }
        }

        /// @brief Get current state (atomic read)
        [[nodiscard]] future_state_enum state() const noexcept {
            return state_.load(std::memory_order_acquire);
        }

        /// @brief Check if result is ready (ready or error)
        [[nodiscard]] bool is_ready() const noexcept {
            auto s = state_.load(std::memory_order_acquire);
            return s == future_state_enum::ready || s == future_state_enum::error;
        }

        /// @brief Check if cancelled
        [[nodiscard]] bool is_cancelled() const noexcept {
            return state_.load(std::memory_order_acquire) == future_state_enum::cancelled;
        }

        /// @brief Check if consumed (result already taken)
        [[nodiscard]] bool is_consumed() const noexcept {
            return state_.load(std::memory_order_acquire) == future_state_enum::consumed;
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

        /// @brief Set result from type-erased rtt (virtual, for handler compatibility)
        /// This is called from handler.ipp which doesn't know the concrete type T
        virtual void set_result_rtt(rtt&& value) noexcept = 0;

#if HAVE_STD_COROUTINES
        /// @brief Resume stored coroutine (if any)
        /// @note Called from behavior_t::resume_if_suspended()
        /// @note Virtual with `final` override for devirtualization
        virtual void resume_coroutine() noexcept = 0;

        /// @brief Check if coroutine is stored
        /// @return true if coroutine_handle is valid
        [[nodiscard]] virtual bool has_coroutine() const noexcept = 0;

        /// @brief Check if stored coroutine is done
        /// @return true if coroutine exists and is done
        [[nodiscard]] virtual bool coroutine_done() const noexcept = 0;
#endif // HAVE_STD_COROUTINES

#ifndef NDEBUG
        [[nodiscard]] uint64_t generation() const noexcept { return generation_; }
#endif

    protected:
        /// @brief Virtual destruction - derived class implements actual deallocation
        virtual void destroy() noexcept = 0;

        pmr::memory_resource* resource_;
        std::atomic<future_state_enum> state_;
        std::atomic<int> refcount_;

#ifndef NDEBUG
        static constexpr uint32_t kMagicAlive = 0xFEEDFACE;
        static constexpr uint32_t kMagicDead = 0xDEADC0DE;

        uint32_t magic_;
        uint64_t generation_;

        static uint64_t next_generation() {
            static std::atomic<uint64_t> counter{0};
            return counter.fetch_add(1, std::memory_order_relaxed);
        }
#endif
    };

    /// @brief Typed future state with result storage
    template<typename T>
    class future_state final : public future_state_base {
    public:
        explicit future_state(pmr::memory_resource* res) noexcept
            : future_state_base(res)
            , result_(res)
#if HAVE_STD_COROUTINES
            , coro_handle_()
            , continuations_(pmr::polymorphic_allocator<unique_function<void()>>(res))
#endif
        {}

        ~future_state() noexcept override {
#if HAVE_STD_COROUTINES
            // Destroy coroutine if exists and not done
            if (coro_handle_ && !coro_handle_.done()) {
                coro_handle_.destroy();
            }
#endif
        }

        /// @brief Set successful result
        void set_result(rtt&& value) noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Use-after-free: set_result() on deleted state!");
#endif
            // CRITICAL FIX: Synchronize non-atomic result_ with atomic state_
            //
            // Problem: result_ is non-atomic, state_ is atomic
            // We need: result_ write happens-before state_ changes to ready
            //
            // Solution: Write result BEFORE CAS with release ordering
            //   1. Write result_ first (may write even if future is cancelled)
            //   2. CAS pending→ready with release ordering
            //   3. Release ordering ensures: result_ write visible to acquire load
            //
            // Safety: If CAS fails (future cancelled), result_ is written but won't be read
            // because reader checks state==ready before accessing result_

            // Write result BEFORE CAS
            result_ = std::move(value);

            // Atomic transition: pending → ready with RELEASE semantics
            // Release ordering creates happens-before with acquire load in is_ready()
            // This ensures: result_ write visible to threads that see state==ready
            auto expected = future_state_enum::pending;
            bool transitioned = state_.compare_exchange_strong(expected, future_state_enum::ready,
                                                std::memory_order_release,  // success: release
                                                std::memory_order_relaxed);  // failure: relaxed

#if HAVE_STD_COROUTINES
            // PHASE 4: Invoke all continuations after result is set
            if (transitioned && !continuations_.empty()) {
                std::cout << "[SET_RESULT] Invoking " << continuations_.size() << " continuations\n";
                // Move continuations out to invoke (avoids issues if continuation adds more continuations)
                auto conts = std::move(continuations_);
                continuations_.clear();

                // Invoke all registered continuations
                for (size_t i = 0; i < conts.size(); ++i) {
                    std::cout << "[SET_RESULT] Invoking continuation #" << i << "\n";
                    if (conts[i]) {
                        conts[i]();
                        std::cout << "[SET_RESULT] Continuation #" << i << " completed\n";
                    }
                }
                std::cout << "[SET_RESULT] All continuations invoked\n";
            } else if (transitioned) {
                std::cout << "[SET_RESULT] Transitioned but no continuations registered\n";
            }
#else
            (void)transitioned;  // Suppress unused warning when coroutines disabled
#endif
        }

        /// @brief Virtual method for type-erased rtt (called from handler.ipp)
        void set_result_rtt(rtt&& value) noexcept override {
            set_result(std::move(value));
        }

        /// @brief Get result reference (caller must ensure is_ready())
        [[nodiscard]] rtt& result() noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Use-after-free: result() on deleted state!");
            auto s = state_.load(std::memory_order_relaxed);
            assert((s == future_state_enum::ready || s == future_state_enum::consumed)
                   && "result() called before set_result()!");
#endif
            return result_;
        }

        /// @brief Get const result reference
        [[nodiscard]] const rtt& result() const noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Use-after-free: result() on deleted state!");
            auto s = state_.load(std::memory_order_relaxed);
            assert((s == future_state_enum::ready || s == future_state_enum::consumed)
                   && "result() called before set_result()!");
#endif
            return result_;
        }

        /// @brief Take result (move out, mark as consumed)
        [[nodiscard]] rtt take_result() noexcept {
            auto expected = future_state_enum::ready;
            bool transitioned = transition(expected, future_state_enum::consumed);
            assert(transitioned && "take_result() called on non-ready state!");
            (void)transitioned;

            return std::move(result_);
        }

#if HAVE_STD_COROUTINES
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
        void set_coroutine(coroutine_handle<void> handle) noexcept {
            coro_handle_ = handle;
        }

        /// @brief Add continuation callback to be invoked when future becomes ready
        /// @param continuation Callback to invoke when result is set
        /// @note If future is already ready, continuation is invoked immediately
        /// @note PHASE 4: Enables non-blocking co_await and then() chains
        void add_continuation(unique_function<void()>&& continuation) {
            // Check if already ready - if so, invoke immediately
            if (is_ready()) {
                std::cout << "[ADD_CONTINUATION] Future already ready, invoking immediately\n";
                if (continuation) {
                    continuation();
                }
                return;
            }

            // Otherwise, store for later invocation in set_result()
            std::cout << "[ADD_CONTINUATION] Storing continuation for later (count=" << continuations_.size() << ")\n";
            continuations_.push_back(std::move(continuation));
        }
#endif // HAVE_STD_COROUTINES

    protected:
        void destroy() noexcept override {
            this->~future_state();
            resource_->deallocate(this, sizeof(future_state<T>), alignof(future_state<T>));
        }

    private:
        rtt result_;
#if HAVE_STD_COROUTINES
        coroutine_handle<void> coro_handle_;  // Stored coroutine (for STATE mode)
        std::vector<unique_function<void()>, pmr::polymorphic_allocator<unique_function<void()>>> continuations_;  // PHASE 4: Continuation callbacks
#endif
    };

    /// @brief Specialization for void (no result storage needed)
    template<>
    class future_state<void> final : public future_state_base {
    public:
        explicit future_state(pmr::memory_resource* res) noexcept
            : future_state_base(res)
#if HAVE_STD_COROUTINES
            , coro_handle_()
            , continuations_(pmr::polymorphic_allocator<unique_function<void()>>(res))
#endif
        {}

        ~future_state() noexcept override {
#if HAVE_STD_COROUTINES
            // Destroy coroutine if exists and not done
            if (coro_handle_ && !coro_handle_.done()) {
                coro_handle_.destroy();
            }
#endif
        }

        /// @brief Mark as ready (void has no result to store)
        void set_ready() noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Use-after-free: set_ready() on deleted state!");
#endif
            auto expected = future_state_enum::pending;
            // Try transition - if it fails, future was cancelled or already processed
            bool transitioned = transition(expected, future_state_enum::ready);

#if HAVE_STD_COROUTINES
            // PHASE 4: Invoke all continuations after becoming ready
            if (transitioned && !continuations_.empty()) {
                // Move continuations out to invoke (avoids issues if continuation adds more continuations)
                auto conts = std::move(continuations_);
                continuations_.clear();

                // Invoke all registered continuations
                for (auto& cont : conts) {
                    if (cont) {
                        cont();
                    }
                }
            }
#else
            (void)transitioned;  // Suppress unused warning when coroutines disabled
#endif
        }

        /// @brief Virtual method for type-erased rtt (called from handler.ipp)
        /// For void specialization, we just mark as ready and ignore the value
        void set_result_rtt(rtt&& /* value */) noexcept override {
            set_ready();
        }

#if HAVE_STD_COROUTINES
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
        void set_coroutine(coroutine_handle<void> handle) noexcept {
            coro_handle_ = handle;
        }

        /// @brief Add continuation callback to be invoked when future becomes ready
        /// @param continuation Callback to invoke when ready
        /// @note If future is already ready, continuation is invoked immediately
        /// @note PHASE 4: Enables non-blocking co_await and then() chains
        void add_continuation(unique_function<void()>&& continuation) {
            // Check if already ready - if so, invoke immediately
            if (is_ready()) {
                if (continuation) {
                    continuation();
                }
                return;
            }

            // Otherwise, store for later invocation in set_ready()
            continuations_.push_back(std::move(continuation));
        }
#endif // HAVE_STD_COROUTINES

    protected:
        void destroy() noexcept override {
            this->~future_state();
            resource_->deallocate(this, sizeof(future_state<void>), alignof(future_state<void>));
        }

    private:
#if HAVE_STD_COROUTINES
        coroutine_handle<void> coro_handle_;  // Stored coroutine (for STATE mode)
        std::vector<unique_function<void()>, pmr::polymorphic_allocator<unique_function<void()>>> continuations_;  // PHASE 4: Continuation callbacks
#endif
    };

}} // namespace actor_zeta::detail