#pragma once

#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/intrusive_ptr.hpp>
#include <actor-zeta/detail/rtt.hpp>

#include <cassert>
#include <chrono>
#include <thread>
#include <utility>

namespace actor_zeta {

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
    public:
        explicit unique_future(pmr::memory_resource* /*res*/) noexcept
            : state_(nullptr)
            , needs_scheduling_(false) {
        }

        unique_future(const unique_future&) = delete;
        unique_future& operator=(const unique_future&) = delete;

        /// @brief Construct from future_state pointer
        /// @param state Future state pointer (ownership transferred)
        /// @param needs_sched true if actor was unblocked (needs scheduling)
        explicit unique_future(detail::future_state<T>* state, bool needs_sched = false) noexcept
            : state_(state)
            , needs_scheduling_(needs_sched) {
        }

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

        /// @brief Blocking get - wait for result with hybrid waiting strategy
        T get() && {
            assert(state_ && "get() on invalid future");

            // Hybrid waiting strategy for optimal performance:
            // 1. Fast spin (0-10): For quick operations (0 CPU overhead)
            // 2. Yield loop (11-100): Medium latency operations
            // 3. Exponential backoff (>100): Slow operations (reduce CPU usage)

            int spin_count = 0;
            constexpr int fast_spin_limit = 10;
            constexpr int yield_limit = 100;

            // Phase 1: Fast spin (no yield, no sleep)
            while (!state_->is_ready() && spin_count < fast_spin_limit) {
                if (state_->is_cancelled()) {
                    state_->release();
                    state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    return T{};
                }
                ++spin_count;
            }

            // Phase 2: Yield loop
            while (!state_->is_ready() && spin_count < yield_limit) {
                if (state_->is_cancelled()) {
                    state_->release();
                    state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    return T{};
                }
                std::this_thread::yield();
                ++spin_count;
            }

            // Phase 3: Exponential backoff with sleep
            auto backoff = std::chrono::microseconds(1);
            constexpr auto max_backoff = std::chrono::microseconds(100);

            while (!state_->is_ready()) {
                if (state_->is_cancelled()) {
                    state_->release();
                    state_ = nullptr;
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
            T result = state_->result().template get<T>(0);

            // Release state
            state_->release();
            state_ = nullptr;

            return result;
        }

        /// @brief Lvalue get() DELETED
        T get() & = delete;

        /// @brief Check if ready
        [[nodiscard]] bool is_ready() const noexcept {
            return state_ && state_->is_ready();
        }

        /// @brief Check if valid
        [[nodiscard]] bool valid() const noexcept {
            return state_ != nullptr;
        }

        /// @brief Check if needs scheduling
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

    private:
        detail::future_state<T>* state_;  // State ownership (null after move or invalid future)
        bool needs_scheduling_;
    };

    /// @brief unique_future<void> specialization - read interface for void async operations
    template<>
    class unique_future<void> final {
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

    private:
        detail::future_state<void>* state_;  // State ownership (null after move or invalid future)
        bool needs_scheduling_;
    };

} // namespace actor_zeta