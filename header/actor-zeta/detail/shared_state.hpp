#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory_resource>
#include <system_error>

#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/future_state.hpp>  // for result_storage<T>
#include <actor-zeta/detail/state_flags.hpp>

namespace actor_zeta::detail {

    // Forward declaration
    template<typename T>
    struct shared_state;

    // Allocator helper
    template<typename T>
    shared_state<T>* allocate_shared_state(std::pmr::memory_resource* resource) {
        assert(resource && "allocate_shared_state: resource must not be null");
        void* mem = resource->allocate(sizeof(shared_state<T>), alignof(shared_state<T>));
        return new (mem) shared_state<T>(resource);
    }

    // Static asserts for lock-free requirements
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
                  "uint8_t must be lock-free for state flags");

    // Primary template: shared_state<T> for non-void types
    template<typename T>
    struct shared_state {
        // Memory resource for deallocation (first for initialization order)
        std::pmr::memory_resource* resource_;

        // Flags (Last-One-Out)
        std::atomic<std::uint8_t> flags_{state_flags::empty};

        // Value storage
        result_storage<T> value_;

        // Continuation (who is waiting) - NOT owning!
        std::atomic<std::coroutine_handle<>> continuation_{nullptr};

        // Error
        std::error_code error_{};

        explicit shared_state(std::pmr::memory_resource* r) noexcept
            : resource_(r)
            , value_(r) {}

        // Non-copyable, non-movable
        shared_state(const shared_state&) = delete;
        shared_state& operator=(const shared_state&) = delete;
        shared_state(shared_state&&) = delete;
        shared_state& operator=(shared_state&&) = delete;

        ~shared_state() noexcept = default;

        // === Write API (from coroutine) ===
        // IMPORTANT: NO resume here! Resume only in final_suspend (Variant B+)

        void set_value(T&& v) noexcept {
            value_.emplace(std::move(v));
            flags_.fetch_or(state_flags::value_set, std::memory_order_release);
            // NO resume - continuation is taken in final_suspend
        }

        void set_value(const T& v) noexcept {
            value_.emplace(v);
            flags_.fetch_or(state_flags::value_set, std::memory_order_release);
        }

        void set_error(std::error_code ec) noexcept {
            error_ = ec;
            flags_.fetch_or(state_flags::error_set, std::memory_order_release);
            // NO resume - continuation is taken in final_suspend
        }

        // === Read API (from future) ===

        [[nodiscard]] bool is_ready() const noexcept {
            // Variant B: check promise_released, not value_set
            return flags_.load(std::memory_order_acquire) & state_flags::promise_released;
        }

        [[nodiscard]] bool has_result() const noexcept {
            // Internal method for await_suspend
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

        // === Continuation API ===

        /// @brief Atomically take the continuation handle
        /// @return The continuation handle, or null handle if none was set
        [[nodiscard]] std::coroutine_handle<> take_continuation() noexcept {
            return continuation_.exchange(nullptr, std::memory_order_acq_rel);
        }

        // === Lifetime API (Last-One-Out) ===

        void release_promise() noexcept {
            auto old = flags_.fetch_or(state_flags::promise_released,
                                       std::memory_order_acq_rel);

#if HAVE_ATOMIC_WAIT
            flags_.notify_all();  // Wake up backport wait() if any
#endif

            if (old & state_flags::future_released) {
                deallocate();  // Last one out
            }
        }

        void release_future() noexcept {
            // Clear continuation - the waiter (future owner) is going away.
            // This prevents final_suspend from resuming a dangling handle.
            continuation_.store(nullptr, std::memory_order_release);

            auto old = flags_.fetch_or(state_flags::future_released,
                                       std::memory_order_acq_rel);
            if (old & state_flags::promise_released) {
                deallocate();  // Last one out
            }
        }

    private:
        void deallocate() noexcept {
            auto* res = resource_;
            this->~shared_state();
            res->deallocate(this, sizeof(shared_state<T>), alignof(shared_state<T>));
        }
    };

    // Explicit specialization for void
    template<>
    struct shared_state<void> {
        // Memory resource for deallocation (first for initialization order)
        std::pmr::memory_resource* resource_;

        std::atomic<std::uint8_t> flags_{state_flags::empty};
        std::atomic<std::coroutine_handle<>> continuation_{nullptr};
        std::error_code error_{};

        // NO value_ - void doesn't store a value

        explicit shared_state(std::pmr::memory_resource* r) noexcept
            : resource_(r) {}

        // Non-copyable, non-movable
        shared_state(const shared_state&) = delete;
        shared_state& operator=(const shared_state&) = delete;
        shared_state(shared_state&&) = delete;
        shared_state& operator=(shared_state&&) = delete;

        ~shared_state() noexcept = default;

        // === Write API ===

        void set_value() noexcept {
            flags_.fetch_or(state_flags::value_set, std::memory_order_release);
        }

        void set_error(std::error_code ec) noexcept {
            error_ = ec;
            flags_.fetch_or(state_flags::error_set, std::memory_order_release);
        }

        // === Read API ===

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

        void get_value() noexcept {
            // void - nothing to return
        }

        void take_value() noexcept {
            // void - nothing to return (analog of get_value for move semantics)
        }

        // === Continuation API ===

        [[nodiscard]] std::coroutine_handle<> take_continuation() noexcept {
            return continuation_.exchange(nullptr, std::memory_order_acq_rel);
        }

        // === Lifetime API (Last-One-Out) ===

        void release_promise() noexcept {
            auto old = flags_.fetch_or(state_flags::promise_released,
                                       std::memory_order_acq_rel);
#if HAVE_ATOMIC_WAIT
            flags_.notify_all();
#endif
            if (old & state_flags::future_released) {
                deallocate();
            }
        }

        void release_future() noexcept {
            // Clear continuation - the waiter (future owner) is going away.
            continuation_.store(nullptr, std::memory_order_release);

            auto old = flags_.fetch_or(state_flags::future_released,
                                       std::memory_order_acq_rel);
            if (old & state_flags::promise_released) {
                deallocate();
            }
        }

    private:
        void deallocate() noexcept {
            auto* res = resource_;
            this->~shared_state();
            res->deallocate(this, sizeof(shared_state<void>), alignof(shared_state<void>));
        }
    };

} // namespace actor_zeta::detail
