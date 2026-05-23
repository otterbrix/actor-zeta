#pragma once

#include <actor-zeta/detail/future_state.hpp>

namespace actor_zeta { namespace detail {

    future_state_base::future_state_base(std::pmr::memory_resource* res) noexcept
        : resource_(res)
        , state_(future_state_enum::pending)
        , refcount_(1)
        , owning_coro_handle_()
        , resume_coro_handle_()
        , owns_coroutine_(false)
#ifndef NDEBUG
        , magic_(kMagicAlive)
        , generation_(next_generation())
#endif
    {
        assert(res != nullptr && "future_state_base: resource must not be null");
    }

    future_state_base::~future_state_base() noexcept {
        if (owns_coroutine_ && owning_coro_handle_) {
            owning_coro_handle_.destroy();
        }
#ifndef NDEBUG
        assert(is_alive() && "Double delete detected!");
        magic_.store(kMagicDead, std::memory_order_release);
#endif
    }

    void future_state_base::add_ref() noexcept {
#ifndef NDEBUG
        assert(is_alive() && "Use-after-free: add_ref() on deleted state!");
        int old_value = refcount_.fetch_add(1, std::memory_order_relaxed);
        assert(old_value > 0 && "Refcount underflow!");
        assert(old_value < 1000000 && "Refcount overflow!");
#else
        refcount_.fetch_add(1, std::memory_order_relaxed);
#endif
    }

    void future_state_base::release() noexcept {
        int old_value = refcount_.fetch_sub(1, std::memory_order_release);
#ifndef NDEBUG
        assert(old_value > 0 && "Refcount underflow!");
#endif
        if (old_value == 1) {
            std::atomic_thread_fence(std::memory_order_acquire);
            destroy();
        }
    }

    bool future_state_base::is_ready() const noexcept {
        auto s = state_.load(std::memory_order_acquire);
        return s == future_state_enum::ready || s == future_state_enum::consumed;
    }

    bool future_state_base::is_failed() const noexcept {
        return state_.load(std::memory_order_acquire) >= future_state_enum::error;
    }

    bool future_state_base::is_error() const noexcept {
        return state_.load(std::memory_order_acquire) == future_state_enum::error;
    }

    bool future_state_base::error(std::error_code ec) noexcept {
        auto expected = future_state_enum::pending;
        if (state_.compare_exchange_strong(expected, future_state_enum::error,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            error_code_ = ec;
            return true;
        }
        return false;
    }

    std::error_code future_state_base::error() const noexcept {
        return error_code_;
    }

    bool future_state_base::is_cancelled() const noexcept {
        return state_.load(std::memory_order_acquire) == future_state_enum::cancelled;
    }

    bool future_state_base::cancelled() noexcept {
        auto expected = future_state_enum::pending;
        if (state_.compare_exchange_strong(expected, future_state_enum::cancelled,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            error_code_ = std::make_error_code(std::errc::operation_canceled);
            return true;
        }
        return false;
    }

    bool future_state_base::is_pending() const noexcept {
        auto s = state_.load(std::memory_order_acquire);
        return s == future_state_enum::pending ||
               s == future_state_enum::setting ||
               s == future_state_enum::consuming;
    }

    std::pmr::memory_resource* future_state_base::memory_resource() const noexcept {
        return resource_;
    }

    void future_state_base::resume_coroutine() noexcept {
        if (owns_coroutine_ && owning_coro_handle_ && !owning_coro_handle_.done()) {
            owning_coro_handle_.resume();
            return;
        }
        if (resume_coro_handle_ && !resume_coro_handle_.done()) {
            resume_coro_handle_.resume();
        }
    }

    void future_state_base::set_coroutine(coroutine_handle<> handle) noexcept {
        resume_coro_handle_ = handle;
    }

    void future_state_base::set_coroutine_owning(coroutine_handle<> handle) noexcept {
        owning_coro_handle_ = handle;
        owns_coroutine_ = true;
    }

    void future_state_base::set_awaiting_on(future_state_base* target) noexcept {
        awaiting_on_ = target;
    }

    future_state_base* future_state_base::get_awaiting_on() const noexcept {
        return awaiting_on_.get();
    }

    void future_state_base::clear_awaiting_on() noexcept {
        awaiting_on_ = nullptr;
    }

    coroutine_handle<> future_state_base::take_continuation() noexcept {
        auto h = resume_coro_handle_;
        resume_coro_handle_ = {};
        return h;
    }

    bool future_state_base::is_available() const noexcept {
        if (state_.load(std::memory_order_acquire) < future_state_enum::ready) {
            return false;
        }
        
        if (owns_coroutine_ && owning_coro_handle_) {
            if (!owning_coro_handle_.done()) {
                return false;  // Not safe to destroy yet!
            }
        }

        return true;
    }

    void future_state_base::try_resume_continuation() noexcept {
        if (!owns_coroutine_ && resume_coro_handle_ && !resume_coro_handle_.done()) {
            auto cont = resume_coro_handle_;
            resume_coro_handle_ = {};
            cont.resume();
        }
    }

    uint8_t future_state_base::load_state_raw() const noexcept {
        return static_cast<uint8_t>(state_.load(std::memory_order_acquire));
    }

    void future_state_base::store_state_raw(uint8_t s) noexcept {
        state_.store(static_cast<future_state_enum>(s), std::memory_order_release);
    }

    bool future_state_base::cas_state_raw(uint8_t& expected, uint8_t desired) noexcept {
        auto exp_enum = static_cast<future_state_enum>(expected);
        bool result = state_.compare_exchange_strong(
            exp_enum,
            static_cast<future_state_enum>(desired),
            std::memory_order_acq_rel,
            std::memory_order_acquire);
        expected = static_cast<uint8_t>(exp_enum);
        return result;
    }

#ifndef NDEBUG
    uint64_t future_state_base::next_generation() {
        static std::atomic<uint64_t> counter{0};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    bool future_state_base::is_alive() const noexcept {
        return magic_.load(std::memory_order_acquire) == kMagicAlive;
    }
#endif

    void intrusive_ptr_add_ref(const future_state_base* p) noexcept {
        const_cast<future_state_base*>(p)->add_ref();
    }

    void intrusive_ptr_release(const future_state_base* p) noexcept {
        const_cast<future_state_base*>(p)->release();
    }

}} // namespace actor_zeta::detail