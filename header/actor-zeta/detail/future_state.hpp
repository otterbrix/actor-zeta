#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <system_error>
#include <thread>
#include <type_traits>

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
        explicit future_state_base(std::pmr::memory_resource* res) noexcept;

        future_state_base(const future_state_base&) = delete;
        future_state_base& operator=(const future_state_base&) = delete;

        virtual ~future_state_base() noexcept;

        void add_ref() noexcept;
        void release() noexcept;

        [[nodiscard]] bool is_ready() const noexcept;
        [[nodiscard]] bool is_failed() const noexcept;
        [[nodiscard]] bool is_error() const noexcept;

        bool error(std::error_code ec) noexcept;
        [[nodiscard]] std::error_code error() const noexcept;

        void wait_until_ready() const noexcept;

        [[nodiscard]] bool is_cancelled() const noexcept;
        bool cancelled() noexcept;

        [[nodiscard]] bool is_pending() const noexcept;

        [[nodiscard]] std::pmr::memory_resource* memory_resource() const noexcept;

        void resume_coroutine() noexcept;
        void set_coroutine(coroutine_handle<> handle) noexcept;
        void set_coroutine_owning(coroutine_handle<> handle) noexcept;

        virtual bool set_forward_target_void(future_state_base* target) noexcept = 0;

        void set_awaiting_on(future_state_base* target) noexcept;
        [[nodiscard]] future_state_base* get_awaiting_on() const noexcept;
        void clear_awaiting_on() noexcept;

        [[nodiscard]] coroutine_handle<> take_continuation() noexcept;

    protected:
        [[nodiscard]] bool is_available() const noexcept;
        void try_resume_continuation() noexcept;

        [[nodiscard]] uint8_t load_state_raw() const noexcept;
        void store_state_raw(uint8_t s) noexcept;
        bool cas_state_raw(uint8_t& expected, uint8_t desired) noexcept;

    public:
#ifndef NDEBUG
        [[nodiscard]] uint64_t generation() const noexcept { return generation_; }
#endif

    protected:
        virtual void destroy() noexcept = 0;

        std::pmr::memory_resource* resource_ = nullptr;
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

        static uint64_t next_generation();
        [[nodiscard]] bool is_alive() const noexcept;
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
            // Note: null check added to satisfy GCC static analyzer which cannot
            // prove resource_ is always initialized through complex coroutine
            // transformations and move semantics (false positive -Wmaybe-uninitialized).
            // See docs/gcc-maybe-uninitialized-false-positive.md
            if (resource_) {
                resource_->deallocate(this, sizeof(future_state<T>), alignof(future_state<T>));
            }
        }

    private:
        [[no_unique_address]] result_storage<T> storage_;
        std::atomic<future_state<T>*> forward_target_;
    };

    void intrusive_ptr_add_ref(const future_state_base* p) noexcept;
    void intrusive_ptr_release(const future_state_base* p) noexcept;

}} // namespace actor_zeta::detail