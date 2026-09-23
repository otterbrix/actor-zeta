#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory_resource>
#include <exception>
#include <system_error>

#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/result_storage.hpp>
#include <actor-zeta/detail/state_flags.hpp>


// __cpp_exceptions (the compiler's answer) and __EXCEPTIONS_DISABLE__ (the CMake intent) must agree.
#if defined(__EXCEPTIONS_DISABLE__) && defined(__cpp_exceptions)
#error "EXCEPTIONS_DISABLE=ON, but the compiler still has exceptions enabled -- check that -fno-exceptions actually reached this translation unit"
#endif

// ODR, accepted deliberately: shared_state carries an exception_ptr only under -fexceptions, so its
// layout differs between modes. Linking objects from DIFFERENT settings is undefined and undiagnosable here.

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
        std::pmr::memory_resource* resource_;
        std::atomic<std::uint8_t> flags_{state_flags::empty};
        result_storage<T> value_;
        std::atomic<std::coroutine_handle<>> continuation_{nullptr};   // non-owning
        std::error_code error_{};
#ifdef __cpp_exceptions
        std::exception_ptr exception_{};
#endif

        explicit shared_state(std::pmr::memory_resource* r) noexcept
            : resource_(r)
            , value_(r) {}

        shared_state(const shared_state&) = delete;
        shared_state& operator=(const shared_state&) = delete;
        shared_state(shared_state&&) = delete;
        shared_state& operator=(shared_state&&) = delete;

        ~shared_state() noexcept = default;

        // Producer side; never resumes the continuation. Template because `void&&` is ill-formed otherwise.
        template<typename U = T>
            requires(!std::is_void_v<T>)
        void set_value(U&& v) noexcept {
            value_.emplace(std::forward<U>(v));
            flags_.fetch_or(state_flags::value_set, std::memory_order_release);
        }

        void set_value() noexcept
            requires(std::is_void_v<T>)
        {
            flags_.fetch_or(state_flags::value_set, std::memory_order_release);
        }

        void set_error(std::error_code ec) noexcept {
            error_ = ec;
            flags_.fetch_or(state_flags::error_set, std::memory_order_release);
        }

#ifdef __cpp_exceptions
        void set_exception(std::exception_ptr ep) noexcept {
            exception_ = ep;
            error_ = std::make_error_code(std::errc::interrupted);
            flags_.fetch_or(state_flags::error_set, std::memory_order_release);
        }

        void rethrow_if_exception() const {
            if ((flags_.load(std::memory_order_acquire) & state_flags::error_set) && exception_) {
                std::rethrow_exception(exception_);
            }
        }
#else
        void rethrow_if_exception() const noexcept {}
#endif

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

        // decltype(auto): `void&` is ill-formed, so reference-ness must come from result_storage<T>::get().
        [[nodiscard]] decltype(auto) get_value() noexcept {
            return value_.get();
        }

        [[nodiscard]] decltype(auto) get_value() const noexcept {
            return value_.get();
        }

        // I2 -- flags are MONOTONIC (only promise_finalizing is ever cleared): taking SETS
        // consumed, since clearing value_set would fire release_promise()'s repair on a consumed value.
        decltype(auto) take_value() noexcept {
            flags_.fetch_or(state_flags::consumed, std::memory_order_release);
            return value_.take();
        }

        // I3 -- the extraction predicate. NOT is_ready(): promise_released means finished, not produced.
        [[nodiscard]] bool holds_value() const noexcept {
            const auto bits = flags_.load(std::memory_order_acquire);
            return (bits & (state_flags::value_set | state_flags::error_set | state_flags::consumed))
                   == state_flags::value_set;
        }


        // Returns true if this call deallocated (future already released; do NOT resume).
        // SETTLED-OUTCOME / I1, TOTALITY: the ONLY writer of promise_released, so error_set rides
        // the same release-ordered RMW when no result exists: `is_ready() => has_result()` at every
        // instant, never "ready, not failed, nothing to take". Plain load: set_value()/set_error() precede.
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
            // Last one out: a producer still finalizing deallocates itself after its double-check.
            bool promise_was_released = old & state_flags::promise_released;
            bool producer_is_finalizing = old & state_flags::promise_finalizing;
            if (promise_was_released && !producer_is_finalizing) {
                deallocate();
            }
        }

        // CAS finalizing off; false means future_released raced in and we deallocated ourselves.
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
            // CAS failed for another reason: the consumer is alive and waiting. `true` is load-bearing --
            // false reads as "state gone" and final_awaiter would destroy the frame and drop the continuation.
            flags_.fetch_and(static_cast<std::uint8_t>(~state_flags::promise_finalizing), std::memory_order_release);
            return true;
        }

    private:
        void deallocate() noexcept {
            auto* res = resource_;
            this->~shared_state();
            res->deallocate(this, sizeof(shared_state), alignof(shared_state));
        }
    };

} // namespace actor_zeta::detail
