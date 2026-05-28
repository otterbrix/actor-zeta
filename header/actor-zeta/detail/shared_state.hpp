#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory_resource>
#include <system_error>

#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/result_storage.hpp>
#include <actor-zeta/detail/state_flags.hpp>

namespace actor_zeta::detail {

    template<typename T>
    struct shared_state;

    template<typename T>
    shared_state<T>* allocate_shared_state(std::pmr::memory_resource* resource) {
        assert(resource && "allocate_shared_state: resource must not be null");
        void* mem = resource->allocate(sizeof(shared_state<T>), alignof(shared_state<T>));
        return new (mem) shared_state<T>(resource);
    }

    static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
                  "uint8_t must be lock-free for state flags");

    template<typename T>
    struct shared_state {
        std::pmr::memory_resource* resource_;             // first for init order
        std::atomic<std::uint8_t> flags_{state_flags::empty};
        result_storage<T> value_;
        std::atomic<std::coroutine_handle<>> continuation_{nullptr};   // non-owning
        std::error_code error_{};

        explicit shared_state(std::pmr::memory_resource* r) noexcept
            : resource_(r)
            , value_(r) {}

        shared_state(const shared_state&) = delete;
        shared_state& operator=(const shared_state&) = delete;
        shared_state(shared_state&&) = delete;
        shared_state& operator=(shared_state&&) = delete;

        ~shared_state() noexcept = default;

        // Write API (from coroutine). Continuation is resumed only in final_suspend.
        void set_value(T&& v) noexcept {
            value_.emplace(std::move(v));
            flags_.fetch_or(state_flags::value_set, std::memory_order_release);
        }

        void set_value(const T& v) noexcept {
            value_.emplace(v);
            flags_.fetch_or(state_flags::value_set, std::memory_order_release);
        }

        void set_error(std::error_code ec) noexcept {
            error_ = ec;
            flags_.fetch_or(state_flags::error_set, std::memory_order_release);
        }

        // Read API (from future).
        [[nodiscard]] bool is_ready() const noexcept {
            return flags_.load(std::memory_order_acquire) & state_flags::promise_released;
        }

        [[nodiscard]] bool has_result() const noexcept {
            return flags_.load(std::memory_order_acquire) & state_flags::result_set;
        }

        [[nodiscard]] bool has_error() const noexcept {
            return flags_.load(std::memory_order_acquire) & state_flags::error_set;
        }

        [[nodiscard]] std::error_code get_error() const noexcept {
            return error_;
        }

        [[nodiscard]] T& get_value() noexcept {
            return value_.get();
        }

        [[nodiscard]] const T& get_value() const noexcept {
            return value_.get();
        }

        [[nodiscard]] T take_value() noexcept {
            return value_.take();
        }


        /// @brief Atomically take the continuation handle
        /// @return The continuation handle, or null handle if none was set
        [[nodiscard]] std::coroutine_handle<> take_continuation() noexcept {
            return continuation_.exchange(nullptr, std::memory_order_acq_rel);
        }


        /// @brief Release the promise side.
        /// @return true if this call deallocated the state (future was already released),
        ///         meaning the continuation should NOT be resumed (cancelled).
        [[nodiscard]] bool release_promise() noexcept {
            auto old = flags_.fetch_or(state_flags::promise_released,std::memory_order_acq_rel);

            if (old & state_flags::future_released) {
                deallocate();  // Last one out
                return true;   // Cancelled - don't resume continuation
            }
            return false;  // Future still alive - safe to resume continuation
        }

        void release_future() noexcept {
            auto old = flags_.fetch_or(state_flags::future_released, std::memory_order_acq_rel);
            // Deallocate only if:
            // - Promise was already released (promise_released set)
            // - AND producer is NOT in final_suspend path (promise_finalizing not set)
            // If producer is finalizing, it will handle deallocation after its double-check.
            bool promise_was_released = old & state_flags::promise_released;
            bool producer_is_finalizing = old & state_flags::promise_finalizing;
            if (promise_was_released && !producer_is_finalizing) {
                deallocate();  // Last one out, and producer is completely done
            }
            // If producer is finalizing, it will see our future_released flag
            // in its double-check and handle deallocation.
        }

        /// @brief Try to complete the finalize phase by clearing the finalizing flag.
        /// Uses CAS to atomically check if future was released concurrently.
        /// @return true if finalizing was cleared (consumer will deallocate later)
        /// @return false if future_released was set (producer should deallocate)
        [[nodiscard]] bool try_complete_finalize() noexcept {
            // Expected: finalizing + released (no future_released)
            std::uint8_t expected = state_flags::promise_finalizing | state_flags::promise_released;
            // Also allow value_set or error_set flags
            std::uint8_t current = flags_.load(std::memory_order_acquire);
            expected = static_cast<std::uint8_t>(current & ~state_flags::future_released);  // Current minus future_released

            // Try to clear finalizing flag
            std::uint8_t desired = static_cast<std::uint8_t>(expected & ~state_flags::promise_finalizing);

            if (flags_.compare_exchange_strong(expected, desired,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                // CAS succeeded: finalizing cleared, future will deallocate later
                return true;
            }

            // CAS failed: check if future_released was set
            if (expected & state_flags::future_released) {
                // Future was released concurrently - we should deallocate
                deallocate();
                return false;
            }

            // Something else changed (shouldn't happen normally)
            // Try clearing finalizing anyway for safety
            flags_.fetch_and(static_cast<std::uint8_t>(~state_flags::promise_finalizing), std::memory_order_release);
            return true;
        }

    private:
        void deallocate() noexcept {
            auto* res = resource_;
            this->~shared_state();
            res->deallocate(this, sizeof(shared_state<T>), alignof(shared_state<T>));
        }
    };

    template<>
    struct shared_state<void> {
        std::pmr::memory_resource* resource_;             // first for init order
        std::atomic<std::uint8_t> flags_{state_flags::empty};
        std::atomic<std::coroutine_handle<>> continuation_{nullptr};
        std::error_code error_{};

        explicit shared_state(std::pmr::memory_resource* r) noexcept
            : resource_(r) {}

        shared_state(const shared_state&) = delete;
        shared_state& operator=(const shared_state&) = delete;
        shared_state(shared_state&&) = delete;
        shared_state& operator=(shared_state&&) = delete;

        ~shared_state() noexcept = default;

        void set_value() noexcept {
            flags_.fetch_or(state_flags::value_set, std::memory_order_release);
        }

        void set_error(std::error_code ec) noexcept {
            error_ = ec;
            flags_.fetch_or(state_flags::error_set, std::memory_order_release);
        }

        [[nodiscard]] bool is_ready() const noexcept {
            return flags_.load(std::memory_order_acquire) & state_flags::promise_released;
        }

        [[nodiscard]] bool has_result() const noexcept {
            return flags_.load(std::memory_order_acquire) & state_flags::result_set;
        }

        [[nodiscard]] bool has_error() const noexcept {
            return flags_.load(std::memory_order_acquire) & state_flags::error_set;
        }

        [[nodiscard]] std::error_code get_error() const noexcept {
            return error_;
        }

        void get_value() noexcept {}
        void take_value() noexcept {}

        [[nodiscard]] std::coroutine_handle<> take_continuation() noexcept {
            return continuation_.exchange(nullptr, std::memory_order_acq_rel);
        }


        /// @brief Release the promise side.
        /// @return true if this call deallocated the state (future was already released),
        ///         meaning the continuation should NOT be resumed (cancelled).
        [[nodiscard]] bool release_promise() noexcept {
            auto old = flags_.fetch_or(state_flags::promise_released,
                                       std::memory_order_acq_rel);
            if (old & state_flags::future_released) {
                deallocate();
                return true;   // Cancelled - don't resume continuation
            }
            return false;  // Future still alive - safe to resume continuation
        }

        void release_future() noexcept {
            auto old = flags_.fetch_or(state_flags::future_released, std::memory_order_acq_rel);
            // Deallocate only if:
            // - Promise was already released (promise_released set)
            // - AND producer is NOT in final_suspend path (promise_finalizing not set)
            // If producer is finalizing, it will handle deallocation after its double-check.
            bool promise_was_released = old & state_flags::promise_released;
            bool producer_is_finalizing = old & state_flags::promise_finalizing;
            if (promise_was_released && !producer_is_finalizing) {
                deallocate();  // Last one out, and producer is completely done
            }
            // If producer is finalizing, it will see our future_released flag
            // in its double-check and handle deallocation.
        }

        /// @brief Try to complete the finalize phase by clearing the finalizing flag.
        /// Uses CAS to atomically check if future was released concurrently.
        /// @return true if finalizing was cleared (consumer will deallocate later)
        /// @return false if future_released was set (producer should deallocate)
        [[nodiscard]] bool try_complete_finalize() noexcept {
            std::uint8_t current = flags_.load(std::memory_order_acquire);
            std::uint8_t expected = static_cast<std::uint8_t>(current & ~state_flags::future_released);
            std::uint8_t desired = static_cast<std::uint8_t>(expected & ~state_flags::promise_finalizing);

            if (flags_.compare_exchange_strong(expected, desired,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                return true;
            }

            if (expected & state_flags::future_released) {
                deallocate();
                return false;
            }

            flags_.fetch_and(static_cast<std::uint8_t>(~state_flags::promise_finalizing), std::memory_order_release);
            return true;
        }

    private:
        void deallocate() noexcept {
            auto* res = resource_;
            this->~shared_state();
            res->deallocate(this, sizeof(shared_state<void>), alignof(shared_state<void>));
        }
    };

} // namespace actor_zeta::detail
