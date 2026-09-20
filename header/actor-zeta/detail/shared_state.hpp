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

        // SETTLED-OUTCOME / I2 -- flags are MONOTONIC. Taking the value SETS a bit
        // rather than clearing value_set: clearing it would make release_promise()'s
        // totality repair fire on a state whose value was legitimately consumed.
        // Nothing in flags_ is ever cleared except promise_finalizing.
        [[nodiscard]] T take_value() noexcept {
            flags_.fetch_or(state_flags::consumed, std::memory_order_release);
            return value_.take();
        }

        // SETTLED-OUTCOME / I3 -- the extraction predicate.
        //
        // "A value exists and has not been taken yet." Deliberately NOT is_ready():
        // that is promise_released, which says the producer finished, not that it
        // produced anything. Reading it as a value gate is the whole bug class this
        // invariant closes.
        [[nodiscard]] bool holds_value() const noexcept {
            const auto bits = flags_.load(std::memory_order_acquire);
            return (bits & (state_flags::value_set | state_flags::error_set | state_flags::consumed))
                   == state_flags::value_set;
        }

        // Raw flag word, for diagnostics only.
        [[nodiscard]] std::uint8_t flags_snapshot() const noexcept {
            return flags_.load(std::memory_order_acquire);
        }


        // Returns true if this call deallocated the state (future already released =>
        // cancelled; the continuation must NOT be resumed).
        //
        // TOTALITY (invariant SETTLED-OUTCOME / I1). This is the ONLY writer of
        // promise_released in the whole library, and promise_released is what
        // is_ready() reports. A promise that releases without ever writing a value or
        // an error therefore produces a future that says "ready", says "not failed",
        // and has nothing to take -- and take_ready()'s assert, the only thing between
        // that and a read of unset storage, is compiled out under NDEBUG.
        //
        // So repair it here, folding error_set into the SAME release-ordered RMW that
        // publishes promise_released. No observer can then see the released bit without
        // a result bit: `is_ready() => has_result()` holds at every instant, on every
        // thread, in every build mode.
        //
        // The plain load is safe: set_value()/set_error() are producer-side and
        // sequenced before this call on the producer's own thread (final_suspend, or
        // ~promise), so no other thread can be writing a result bit concurrently.
        [[nodiscard]] bool release_promise() noexcept {
            std::uint8_t bits = state_flags::promise_released;
            if ((flags_.load(std::memory_order_acquire) & state_flags::result_set) == 0) {
                error_ = std::make_error_code(std::errc::state_not_recoverable);
                bits = static_cast<std::uint8_t>(bits | state_flags::error_set);
            }

            auto old = flags_.fetch_or(bits, std::memory_order_acq_rel);

            if (old & state_flags::future_released) {
                deallocate();
                return true;
            }
            return false;
        }

        void release_future() noexcept {
            auto old = flags_.fetch_or(state_flags::future_released, std::memory_order_acq_rel);
            // Last-One-Out: deallocate only if the promise is fully done; if the producer
            // is in its final_suspend path (promise_finalizing set), it will deallocate
            // after its double-check.
            bool promise_was_released = old & state_flags::promise_released;
            bool producer_is_finalizing = old & state_flags::promise_finalizing;
            if (promise_was_released && !producer_is_finalizing) {
                deallocate();
            }
        }

        // CAS the finalizing flag off; returns false if future_released raced in
        // (then we deallocate ourselves).
        [[nodiscard]] bool try_complete_finalize() noexcept {
            std::uint8_t current  = flags_.load(std::memory_order_acquire);
            std::uint8_t expected = static_cast<std::uint8_t>(current & ~state_flags::future_released);
            std::uint8_t desired  = static_cast<std::uint8_t>(expected & ~state_flags::promise_finalizing);

            if (flags_.compare_exchange_strong(expected, desired,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                return true;
            }
            if (expected & state_flags::future_released) {
                deallocate();
                return false;
            }
            // Defensive: clear finalizing on unexpected race.
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

        // See the T specialization: monotonic flags, I2.
        void take_value() noexcept {
            flags_.fetch_or(state_flags::consumed, std::memory_order_release);
        }

        // SETTLED-OUTCOME / I3 -- the extraction predicate.
        //
        // "A value exists and has not been taken yet." Deliberately NOT is_ready():
        // that is promise_released, which says the producer finished, not that it
        // produced anything. Reading it as a value gate is the whole bug class this
        // invariant closes.
        [[nodiscard]] bool holds_value() const noexcept {
            const auto bits = flags_.load(std::memory_order_acquire);
            return (bits & (state_flags::value_set | state_flags::error_set | state_flags::consumed))
                   == state_flags::value_set;
        }

        // Raw flag word, for diagnostics only.
        [[nodiscard]] std::uint8_t flags_snapshot() const noexcept {
            return flags_.load(std::memory_order_acquire);
        }


        // Returns true if this call deallocated the state (future already released =>
        // cancelled; the continuation must NOT be resumed).
        //
        // TOTALITY (invariant SETTLED-OUTCOME / I1). This is the ONLY writer of
        // promise_released in the whole library, and promise_released is what
        // is_ready() reports. A promise that releases without ever writing a value or
        // an error therefore produces a future that says "ready", says "not failed",
        // and has nothing to take -- and take_ready()'s assert, the only thing between
        // that and a read of unset storage, is compiled out under NDEBUG.
        //
        // So repair it here, folding error_set into the SAME release-ordered RMW that
        // publishes promise_released. No observer can then see the released bit without
        // a result bit: `is_ready() => has_result()` holds at every instant, on every
        // thread, in every build mode.
        //
        // The plain load is safe: set_value()/set_error() are producer-side and
        // sequenced before this call on the producer's own thread (final_suspend, or
        // ~promise), so no other thread can be writing a result bit concurrently.
        [[nodiscard]] bool release_promise() noexcept {
            std::uint8_t bits = state_flags::promise_released;
            if ((flags_.load(std::memory_order_acquire) & state_flags::result_set) == 0) {
                error_ = std::make_error_code(std::errc::state_not_recoverable);
                bits = static_cast<std::uint8_t>(bits | state_flags::error_set);
            }

            auto old = flags_.fetch_or(bits, std::memory_order_acq_rel);
            if (old & state_flags::future_released) {
                deallocate();
                return true;   // Cancelled - don't resume continuation
            }
            return false;  // Future still alive - safe to resume continuation
        }

        void release_future() noexcept {
            auto old = flags_.fetch_or(state_flags::future_released, std::memory_order_acq_rel);
            // Last-One-Out: deallocate only if the promise is fully done; if the producer
            // is in its final_suspend path (promise_finalizing set), it will deallocate
            // after its double-check.
            bool promise_was_released = old & state_flags::promise_released;
            bool producer_is_finalizing = old & state_flags::promise_finalizing;
            if (promise_was_released && !producer_is_finalizing) {
                deallocate();
            }
        }

        // CAS the finalizing flag off; returns false if future_released raced in
        // (then we deallocate ourselves).
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
