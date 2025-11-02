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
            , needs_scheduling_(false)
            , cancellation_token_() {
        }

        unique_future(const unique_future&) = delete;
        unique_future& operator=(const unique_future&) = delete;

        /// @brief Construct from future_state pointer
        /// @param state Future state pointer (ownership transferred)
        /// @param needs_sched true if actor was unblocked (needs scheduling)
        explicit unique_future(detail::future_state<T>* state, bool needs_sched = false) noexcept
            : state_(state)
            , needs_scheduling_(needs_sched)
            , cancellation_token_(make_counted<detail::cancellation_token>()) {
        }

        /// @brief Move constructor
        unique_future(unique_future&& other) noexcept
            : state_(other.state_)
            , needs_scheduling_(other.needs_scheduling_)
            , cancellation_token_(std::move(other.cancellation_token_)) {
            other.state_ = nullptr;
            other.needs_scheduling_ = false;
        }

        /// @brief Move assignment
        unique_future& operator=(unique_future&& other) noexcept {
            if (this != &other) {
                // RAII cancellation before releasing
                if (state_ && !state_->is_ready() && cancellation_token_) {
                    cancellation_token_->cancel();
                }

                // Release current state if any
                if (state_) {
                    state_->release();
                }

                // Transfer ownership
                state_ = other.state_;
                needs_scheduling_ = other.needs_scheduling_;
                cancellation_token_ = std::move(other.cancellation_token_);

                other.state_ = nullptr;
                other.needs_scheduling_ = false;
            }
            return *this;
        }

        /// @brief Destructor - RAII cancellation + releases state reference
        ~unique_future() noexcept {
            // RAII cancellation: if future is destroyed before get() → cancel
            // TEMPORARY DEBUG: Disable RAII cancellation to test
            // if (state_ && !state_->is_ready() && cancellation_token_) {
            //     cancellation_token_->cancel();
            // }

            if (state_) {
                state_->release();
            }
        }

        /// @brief Blocking get - wait for result with exponential backoff
        T get() && {
            assert(state_ && "get() on invalid future");

            // Exponential backoff waiting strategy
            auto backoff = std::chrono::microseconds(1);
            constexpr auto max_backoff = std::chrono::microseconds(1000);

            while (!state_->is_ready()) {
                // Check cancellation
                if (cancellation_token_ && cancellation_token_->is_cancelled()) {
                    state_->release();
                    state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    // In production: return default-constructed T or throw
                    return T{};
                }

                std::this_thread::sleep_for(backoff);
                if (backoff < max_backoff) {
                    backoff *= 2;
                }
            }

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
            if (cancellation_token_) {
                cancellation_token_->cancel();
            }
        }

        /// @brief Check if cancelled
        [[nodiscard]] bool is_cancelled() const noexcept {
            return cancellation_token_ && cancellation_token_->is_cancelled();
        }

        /// @brief Get cancellation token (for sharing with message)
        [[nodiscard]] intrusive_ptr<detail::cancellation_token> get_cancellation_token() const noexcept {
            return cancellation_token_;
        }

        /// @brief Set cancellation token (for enqueue_impl to share token with message)
        void set_cancellation_token(intrusive_ptr<detail::cancellation_token> token) noexcept {
            cancellation_token_ = std::move(token);
        }

    private:
        detail::future_state<T>* state_;  // State ownership (null after move or invalid future)
        bool needs_scheduling_;
        intrusive_ptr<detail::cancellation_token> cancellation_token_;  // Shared cancellation token
    };

    /// @brief unique_future<void> specialization - read interface for void async operations
    template<>
    class unique_future<void> final {
    public:
        /// @brief Constructor for invalid future - requires memory_resource
        explicit unique_future(pmr::memory_resource* /*res*/) noexcept
            : state_(nullptr)
            , needs_scheduling_(false)
            , cancellation_token_() {
        }

        /// @brief Construct from future_state pointer
        /// @param state Future state pointer (ownership transferred)
        /// @param needs_sched true if actor was unblocked (needs scheduling)
        explicit unique_future(detail::future_state<void>* state, bool needs_sched = false) noexcept
            : state_(state)
            , needs_scheduling_(needs_sched)
            , cancellation_token_(make_counted<detail::cancellation_token>()) {
        }

        unique_future(const unique_future&) = delete;
        unique_future& operator=(const unique_future&) = delete;

        /// @brief Move constructor
        unique_future(unique_future&& other) noexcept
            : state_(other.state_)
            , needs_scheduling_(other.needs_scheduling_)
            , cancellation_token_(std::move(other.cancellation_token_)) {
            other.state_ = nullptr;
            other.needs_scheduling_ = false;
        }

        /// @brief Move assignment
        unique_future& operator=(unique_future&& other) noexcept {
            if (this != &other) {
                // RAII cancellation before releasing
                if (state_ && !state_->is_ready() && cancellation_token_) {
                    cancellation_token_->cancel();
                }

                // Release current state if any
                if (state_) {
                    state_->release();
                }

                // Transfer ownership
                state_ = other.state_;
                needs_scheduling_ = other.needs_scheduling_;
                cancellation_token_ = std::move(other.cancellation_token_);

                other.state_ = nullptr;
                other.needs_scheduling_ = false;
            }
            return *this;
        }

        /// @brief Destructor - RAII cancellation + releases state reference
        ~unique_future() noexcept {
            // RAII cancellation: if future is destroyed before get() → cancel
            // TEMPORARY DEBUG: Disable RAII cancellation to test
            // if (state_ && !state_->is_ready() && cancellation_token_) {
            //     cancellation_token_->cancel();
            // }

            if (state_) {
                state_->release();
            }
        }

        /// @brief Blocking get - wait for completion with exponential backoff
        void get() && {
            assert(state_ && "get() on invalid future");

            auto backoff = std::chrono::microseconds(1);
            constexpr auto max_backoff = std::chrono::microseconds(1000);

            while (!state_->is_ready()) {
                // Check cancellation
                if (cancellation_token_ && cancellation_token_->is_cancelled()) {
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
            if (cancellation_token_) {
                cancellation_token_->cancel();
            }
        }

        /// @brief Check if cancelled
        [[nodiscard]] bool is_cancelled() const noexcept {
            return cancellation_token_ && cancellation_token_->is_cancelled();
        }

        /// @brief Get cancellation token (for sharing with message)
        [[nodiscard]] intrusive_ptr<detail::cancellation_token> get_cancellation_token() const noexcept {
            return cancellation_token_;
        }

        /// @brief Set cancellation token (for enqueue_impl to share token with message)
        void set_cancellation_token(intrusive_ptr<detail::cancellation_token> token) noexcept {
            cancellation_token_ = std::move(token);
        }

    private:
        detail::future_state<void>* state_;  // State ownership (null after move or invalid future)
        bool needs_scheduling_;
        intrusive_ptr<detail::cancellation_token> cancellation_token_;  // Shared cancellation token
    };

} // namespace actor_zeta