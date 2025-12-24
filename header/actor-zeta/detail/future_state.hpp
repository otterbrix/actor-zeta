#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <system_error>
#include <thread>
#include <type_traits>

#include <actor-zeta/config.hpp>
#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/ignore_unused.hpp>
#include <actor-zeta/detail/intrusive_ptr.hpp>

namespace actor_zeta { namespace detail {

    template<typename T>
    inline constexpr bool is_trivially_move_constructible_and_destructible_v =
        std::is_trivially_move_constructible_v<T> &&
        std::is_trivially_destructible_v<T>;

    enum class future_state_enum : uint8_t {
        invalid = 0,
        pending = 1,
        setting = 2,
        consuming = 3,
        ready = 4,
        consumed = 5,
        error = 6,
        cancelled = 7,
    };


    namespace future_states {
        constexpr uint8_t invalid   = 0;
        constexpr uint8_t pending   = 1;
        constexpr uint8_t setting   = 2;
        constexpr uint8_t consuming = 3;
        constexpr uint8_t ready     = 4;
        constexpr uint8_t consumed  = 5;
        constexpr uint8_t error     = 6;
        constexpr uint8_t cancelled = 7;
    } // namespace future_states

    class future_state_base {
    public:
        explicit future_state_base(std::pmr::memory_resource* res) noexcept
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
        }

        future_state_base(const future_state_base&) = delete;
        future_state_base& operator=(const future_state_base&) = delete;

        virtual ~future_state_base() noexcept {
            if (owns_coroutine_ && owning_coro_handle_) {
                owning_coro_handle_.destroy();
            }
#ifndef NDEBUG
            assert(is_alive() && "Double delete detected!");
            magic_.store(kMagicDead, std::memory_order_release);
#endif
        }

        void add_ref() noexcept {
#ifndef NDEBUG
            assert(is_alive() && "Use-after-free: add_ref() on deleted state!");
            int old_value = refcount_.fetch_add(1, std::memory_order_relaxed);
            assert(old_value > 0 && "Refcount underflow!");
            assert(old_value < 1000000 && "Refcount overflow!");
#else
            refcount_.fetch_add(1, std::memory_order_relaxed);
#endif
        }

        void release() noexcept {
            int old_value = refcount_.fetch_sub(1, std::memory_order_release);
#ifndef NDEBUG
            assert(old_value > 0 && "Refcount underflow!");
#endif
            if (old_value == 1) {
                std::atomic_thread_fence(std::memory_order_acquire);
                destroy();
            }
        }

        [[nodiscard]] bool is_ready() const noexcept {
            auto s = state_.load(std::memory_order_acquire);
            return s == future_state_enum::ready || s == future_state_enum::consumed;
        }

        [[nodiscard]] bool is_failed() const noexcept {
            return state_.load(std::memory_order_acquire) >= future_state_enum::error;
        }

        [[nodiscard]] bool is_error() const noexcept {
            return state_.load(std::memory_order_acquire) == future_state_enum::error;
        }

        bool error(std::error_code ec) noexcept {
            auto expected = future_state_enum::pending;
            if (state_.compare_exchange_strong(expected, future_state_enum::error,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                error_code_ = ec;
#if HAVE_ATOMIC_WAIT
                state_.notify_one();
#endif
                return true;
            }
            return false;
        }

        [[nodiscard]] std::error_code error() const noexcept {
            return error_code_;
        }

        void wait_until_ready() const noexcept {
#if HAVE_ATOMIC_WAIT
            while (!is_available()) {
                auto current = state_.load(std::memory_order_acquire);
                if (current == future_state_enum::pending ||
                    current == future_state_enum::setting) {
                    state_.wait(current, std::memory_order_acquire);
                }
            }
#else
            int spin_count = 0;
            constexpr int yield_limit = 100;
            while (!is_available()) {
                if (spin_count < yield_limit) {
                    std::this_thread::yield();
                    ++spin_count;
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
#endif
        }

        [[nodiscard]] bool is_cancelled() const noexcept {
            return state_.load(std::memory_order_acquire) == future_state_enum::cancelled;
        }

        bool cancelled() noexcept {
            auto expected = future_state_enum::pending;
            if (state_.compare_exchange_strong(expected, future_state_enum::cancelled,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                error_code_ = std::make_error_code(std::errc::operation_canceled);
#if HAVE_ATOMIC_WAIT
                state_.notify_one();
#endif
                return true;
            }
            return false;
        }

        [[nodiscard]] bool is_pending() const noexcept {
            auto s = state_.load(std::memory_order_acquire);
            return s == future_state_enum::pending ||
                   s == future_state_enum::setting ||
                   s == future_state_enum::consuming;
        }

        [[nodiscard]] std::pmr::memory_resource* memory_resource() const noexcept {
            return resource_;
        }

        void resume_coroutine() noexcept {
            if (owns_coroutine_ && owning_coro_handle_ && !owning_coro_handle_.done()) {
                owning_coro_handle_.resume();
                return;
            }
            if (resume_coro_handle_ && !resume_coro_handle_.done()) {
                resume_coro_handle_.resume();
            }
        }

        void set_coroutine(coroutine_handle<> handle) noexcept {
            resume_coro_handle_ = handle;
        }

        void set_coroutine_owning(coroutine_handle<> handle) noexcept {
            owning_coro_handle_ = handle;
            owns_coroutine_ = true;
        }

        virtual bool set_forward_target_void(future_state_base* target) noexcept = 0;

        void set_awaiting_on(future_state_base* target) noexcept {
            awaiting_on_ = target;
        }

        [[nodiscard]] future_state_base* get_awaiting_on() const noexcept {
            return awaiting_on_.get();
        }

        void clear_awaiting_on() noexcept {
            awaiting_on_ = nullptr;
        }

        [[nodiscard]] coroutine_handle<> take_continuation() noexcept {
            auto h = resume_coro_handle_;
            resume_coro_handle_ = {};
            return h;
        }

    protected:
        [[nodiscard]] bool is_available() const noexcept {
            return state_.load(std::memory_order_acquire) >= future_state_enum::ready;
        }

        void try_resume_continuation() noexcept {
            if (!owns_coroutine_ && resume_coro_handle_ && !resume_coro_handle_.done()) {
                auto cont = resume_coro_handle_;
                resume_coro_handle_ = {};
                cont.resume();
            }
        }


        [[nodiscard]] uint8_t load_state_raw() const noexcept {
            return static_cast<uint8_t>(state_.load(std::memory_order_acquire));
        }

        void store_state_raw(uint8_t s) noexcept {
            state_.store(static_cast<future_state_enum>(s), std::memory_order_release);
#if HAVE_ATOMIC_WAIT
            state_.notify_one();
#endif
        }

        bool cas_state_raw(uint8_t& expected, uint8_t desired) noexcept {
            auto exp_enum = static_cast<future_state_enum>(expected);
            bool result = state_.compare_exchange_strong(
                exp_enum,
                static_cast<future_state_enum>(desired),
                std::memory_order_acq_rel,
                std::memory_order_acquire);
            expected = static_cast<uint8_t>(exp_enum);
            return result;
        }

    public:
#ifndef NDEBUG
        [[nodiscard]] uint64_t generation() const noexcept { return generation_; }
#endif

    protected:
        virtual void destroy() noexcept = 0;

        std::pmr::memory_resource* resource_;
        std::atomic<future_state_enum> state_;
        std::atomic<int> refcount_;
        intrusive_ptr<future_state_base> awaiting_on_;
        coroutine_handle<void> owning_coro_handle_;
        coroutine_handle<void> resume_coro_handle_;
        bool owns_coroutine_;
        std::error_code error_code_;

#ifndef NDEBUG
        static constexpr uint32_t kMagicAlive = 0xFEEDFACE;
        static constexpr uint32_t kMagicDead = 0xDEADC0DE;
        mutable std::atomic<uint32_t> magic_;
        uint64_t generation_;

        static uint64_t next_generation() {
            static std::atomic<uint64_t> counter{0};
            return counter.fetch_add(1, std::memory_order_relaxed);
        }

        [[nodiscard]] bool is_alive() const noexcept {
            return magic_.load(std::memory_order_acquire) == kMagicAlive;
        }
#endif
    };

    template<typename T>
    struct result_storage {
        union storage_t {
            char dummy_;
            T value_;
            storage_t() noexcept
                : dummy_() {}
            ~storage_t() {}
        } storage_;

        bool has_value_ = false;

#ifndef NDEBUG
        bool was_moved_from_ = false;
#endif

        result_storage() noexcept = default;

        explicit result_storage(std::pmr::memory_resource*) noexcept
            : storage_()
            , has_value_(false)
#ifndef NDEBUG
            , was_moved_from_(false)
#endif
        {
        }

        ~result_storage() noexcept {
            if (has_value_) {
                storage_.value_.~T();
                has_value_ = false;
            }
        }

        result_storage(const result_storage&) = delete;
        result_storage& operator=(const result_storage&) = delete;

        result_storage(result_storage&& other) noexcept
            : has_value_(false)
#ifndef NDEBUG
            , was_moved_from_(false)
#endif
        {
            assert(!other.was_moved_from_ && "Move from already moved-from storage!");

            if (other.has_value_) {
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
                if (has_value_) {
                    if constexpr (!is_trivially_move_constructible_and_destructible_v<T>) {
                        storage_.value_.~T();
                    }
                    has_value_ = false;
                }

                if (other.has_value_) {
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

        template<typename... Args>
        void emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
            assert(!was_moved_from_ && "emplace() on moved-from storage!");
            assert(!has_value_ && "Double emplace() - value already set!");

            new (&storage_.value_) T(std::forward<Args>(args)...);
            has_value_ = true;
        }

        [[nodiscard]] T take() noexcept {
            assert(!was_moved_from_ && "take() on moved-from storage!");
            assert(has_value_ && "take() from empty storage!");

            has_value_ = false;

            if constexpr (is_trivially_move_constructible_and_destructible_v<T>) {
                return storage_.value_;
            } else {
                T result = std::move(storage_.value_);
                storage_.value_.~T();
                return result;
            }
        }

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

        [[nodiscard]] bool empty() const noexcept {
            assert(!was_moved_from_ && "empty() on moved-from storage!");
            return !has_value_;
        }

        [[nodiscard]] bool has_value() const noexcept {
            assert(!was_moved_from_ && "has_value() on moved-from storage!");
            return has_value_;
        }
    };

    template<>
    struct result_storage<void> {
        explicit result_storage(std::pmr::memory_resource*) noexcept {}

        result_storage() noexcept = default;
        result_storage(const result_storage&) = default;
        result_storage(result_storage&&) noexcept = default;
        result_storage& operator=(const result_storage&) = default;
        result_storage& operator=(result_storage&&) noexcept = default;
    };

    template<typename T>
    class future_state final : public future_state_base {
    private:
        static constexpr bool has_result = !std::is_void_v<T>;

    public:
        explicit future_state(std::pmr::memory_resource* res) noexcept
            : future_state_base(res)
            , storage_(res)
            , forward_target_(nullptr) {}

        ~future_state() noexcept override {
            auto* target = forward_target_.load(std::memory_order_acquire);
            if (target) {
                target->release();
            }

            if constexpr (has_result) {
                auto s = state_.load(std::memory_order_acquire);
                if (s == future_state_enum::setting || s == future_state_enum::consuming) {
                    int spin_count = 0;
                    constexpr int fast_spin_limit = 10;
                    while ((s == future_state_enum::setting || s == future_state_enum::consuming) && spin_count < fast_spin_limit) {
                        ++spin_count;
                        s = state_.load(std::memory_order_acquire);
                    }

                    while (s == future_state_enum::setting || s == future_state_enum::consuming) {
                        std::this_thread::yield();
                        s = state_.load(std::memory_order_acquire);
                    }
                }
            }
        }

        bool set_forward_target(future_state<T>* target) noexcept {
            assert(target != nullptr && "Forward target cannot be null!");
            assert(target != this && "Self-forwarding creates infinite loop!");

            target->add_ref();
            future_state<T>* expected = nullptr;
            if (!forward_target_.compare_exchange_strong(expected, target,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
                target->release();
                return false;
            }

            auto s = state_.load(std::memory_order_acquire);
            if (s == future_state_enum::ready || s == future_state_enum::consumed) {
                if constexpr (has_result) {
                    if (storage_.has_value()) {
                        target->set_value(storage_.take());
                    }
                } else {
                    target->set_ready();
                }
            }
            return true;
        }

        template<typename U = T>
            requires(!std::is_void_v<U>)
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
                target->set_value(std::forward<U>(value));
            } else {
                storage_.emplace(std::forward<U>(value));
            }

            state_.store(future_state_enum::ready, std::memory_order_release);

#if HAVE_ATOMIC_WAIT
            state_.notify_one();
#endif
            try_resume_continuation();
        }

        template<typename U = T>
            requires(!std::is_void_v<U>)
        [[nodiscard]] U& get_value() noexcept {
#ifndef NDEBUG
            assert(is_alive() && "Use-after-free: get_value() on deleted state!");
            auto s = state_.load(std::memory_order_relaxed);
            assert((s == future_state_enum::ready || s == future_state_enum::consumed) && "get_value() called before set_value()!");
#endif
            return storage_.get();
        }

        template<typename U = T>
            requires(!std::is_void_v<U>)
        [[nodiscard]] const U& get_value() const noexcept {
#ifndef NDEBUG
            assert(is_alive() && "Use-after-free: get_value() on deleted state!");
            auto s = state_.load(std::memory_order_relaxed);
            assert((s == future_state_enum::ready || s == future_state_enum::consumed) && "get_value() called before set_value()!");
#endif
            return storage_.get();
        }

        template<typename U = T>
            requires(!std::is_void_v<U>)
        [[nodiscard]] U take_value() noexcept {
            auto expected = future_state_enum::ready;
            bool transitioned = state_.compare_exchange_strong(expected, future_state_enum::consuming,
                                                                std::memory_order_acq_rel, std::memory_order_acquire);
            assert(transitioned && "take_value() called on non-ready state!");
            ignore_unused(transitioned);

            U result = storage_.take();
            state_.store(future_state_enum::consumed, std::memory_order_release);
            return result;
        }

        void set_ready() noexcept
            requires(std::is_void_v<T>)
        {
#ifndef NDEBUG
            assert(is_alive() && "Use-after-free: set_ready() on deleted state!");
#endif
            auto expected = future_state_enum::pending;
            if (!state_.compare_exchange_strong(expected, future_state_enum::setting,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
                return;
            }

            auto* target = forward_target_.load(std::memory_order_acquire);
            if (target) {
                target->set_ready();
            }
            state_.store(future_state_enum::ready, std::memory_order_release);

#if HAVE_ATOMIC_WAIT
            state_.notify_one();
#endif
            try_resume_continuation();
        }

        bool set_forward_target_void(future_state_base* target) noexcept override {
            if constexpr (std::is_void_v<T>) {
                return set_forward_target(static_cast<future_state<void>*>(target));
            } else {
                assert(target != nullptr);
                assert(target != this);
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
        std::atomic<future_state<T>*> forward_target_;
    };

    inline void intrusive_ptr_add_ref(const future_state_base* p) noexcept {
        const_cast<future_state_base*>(p)->add_ref();
    }

    inline void intrusive_ptr_release(const future_state_base* p) noexcept {
        const_cast<future_state_base*>(p)->release();
    }

}} // namespace actor_zeta::detail