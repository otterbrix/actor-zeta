#pragma once

#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/ref_counted.hpp>
#include <actor-zeta/detail/rtt.hpp>
#include <atomic>
#include <cassert>
#include <cstdint>

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

    /// @brief Cancellation token with intrusive refcounting
    /// Shared between message and future via intrusive_ptr
    /// Allows RAII-based cancellation when future is destroyed
    class cancellation_token final : public ref_counted {
    public:
        cancellation_token() = default;

        /// @brief Request cancellation (can be called from any thread)
        void cancel() noexcept {
            cancelled_.store(true, std::memory_order_release);
        }

        /// @brief Check if cancelled
        [[nodiscard]] bool is_cancelled() const noexcept {
            return cancelled_.load(std::memory_order_acquire);
        }

        /// @brief Reset cancellation state (for reuse in pools)
        void reset() noexcept {
            cancelled_.store(false, std::memory_order_release);
        }

    private:
        std::atomic<bool> cancelled_{false};
    };

    /// @brief Base class for future state (type-erased part)
    /// Replaces slot_refcount with improved state management
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
            int old_count = refcount_.load(std::memory_order_relaxed);
            assert(old_count > 0 && "Refcount underflow!");
            assert(old_count < 10000 && "Refcount overflow!");
#endif
            refcount_.fetch_add(1, std::memory_order_relaxed);
        }

        /// @brief Decrement reference count and deallocate if zero
        void release() noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Double release detected!");
            int old_count = refcount_.load(std::memory_order_relaxed);
            assert(old_count > 0 && "Refcount underflow!");
#endif
            if (refcount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
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
            , result_(res) {}

        ~future_state() noexcept override = default;

        /// @brief Set successful result
        void set_result(rtt&& value) noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Use-after-free: set_result() on deleted state!");
            auto current = state_.load(std::memory_order_relaxed);
            assert(current == future_state_enum::pending && "set_result() called on non-pending state!");
#endif
            result_ = std::move(value);

            // Atomic transition: pending → ready
            auto expected = future_state_enum::pending;
            if (!transition(expected, future_state_enum::ready)) {
                // Transition failed - state changed concurrently (e.g., cancelled)
#ifndef NDEBUG
                assert(false && "Concurrent state modification detected in set_result()!");
#endif
            }
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

    protected:
        void destroy() noexcept override {
            this->~future_state();
            resource_->deallocate(this, sizeof(future_state<T>), alignof(future_state<T>));
        }

    private:
        rtt result_;
    };

    /// @brief Specialization for void (no result storage needed)
    template<>
    class future_state<void> final : public future_state_base {
    public:
        explicit future_state(pmr::memory_resource* res) noexcept
            : future_state_base(res) {}

        ~future_state() noexcept override = default;

        /// @brief Mark as ready (void has no result to store)
        void set_ready() noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Use-after-free: set_ready() on deleted state!");
#endif
            auto expected = future_state_enum::pending;
            bool transitioned = transition(expected, future_state_enum::ready);
            assert(transitioned && "set_ready() called on non-pending state!");
            (void)transitioned;
        }

        /// @brief Virtual method for type-erased rtt (called from handler.ipp)
        /// For void specialization, we just mark as ready and ignore the value
        void set_result_rtt(rtt&& /* value */) noexcept override {
            set_ready();
        }

    protected:
        void destroy() noexcept override {
            this->~future_state();
            resource_->deallocate(this, sizeof(future_state<void>), alignof(future_state<void>));
        }
    };

}} // namespace actor_zeta::detail