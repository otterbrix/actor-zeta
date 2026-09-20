#pragma once

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory_resource>
#include <type_traits>
#include <utility>

namespace actor_zeta { namespace detail {

    // Asking a valueless storage for its value has no correct answer: T take() cannot
    // return "nothing", and the union member was never made active, so reading it is
    // undefined behaviour -- silently, in Release. Refuse instead, unconditionally and
    // in every build. An assert would not do: it is exactly NDEBUG that turns this into
    // UB, and NDEBUG is what ships.
    [[noreturn]] inline void refuse_valueless_extraction(const char* where) noexcept {
        std::fprintf(stderr,
                     "actor-zeta: %s on a future that holds no value.\n"
                     "  A promise can be released without ever producing one (a send() to a\n"
                     "  closing mailbox cancels it), and is_ready() reports the released bit,\n"
                     "  not the presence of a value. Gate on failed() before extracting.\n",
                     where);
        std::abort();
    }

    template<typename T>
    inline constexpr bool is_trivially_move_constructible_and_destructible_v =
        std::is_trivially_move_constructible_v<T> &&
        std::is_trivially_destructible_v<T>;

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
            if (!has_value_) {
                refuse_valueless_extraction("take()");
            }

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
            if (!has_value_) {
                refuse_valueless_extraction("get()");
            }
            return storage_.value_;
        }

        [[nodiscard]] const T& get() const noexcept {
            assert(!was_moved_from_ && "get() on moved-from storage!");
            if (!has_value_) {
                refuse_valueless_extraction("get()");
            }
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

    // Nothing to store, but the same shape as result_storage<T>: shared_state<T> is
    // one template for every T, and without these it would need a second copy of
    // itself just to avoid naming take()/get() on void.
    template<>
    struct result_storage<void> {
        explicit result_storage(std::pmr::memory_resource*) noexcept {}

        result_storage() noexcept = default;
        result_storage(const result_storage&) = default;
        result_storage(result_storage&&) noexcept = default;
        result_storage& operator=(const result_storage&) = default;
        result_storage& operator=(result_storage&&) noexcept = default;

        void emplace() noexcept {}

        // `return <void expression>;` in a void function is legal, which is what lets
        // shared_state::take_value() stay a single body.
        void take() noexcept {}
        void get() noexcept {}
        void get() const noexcept {}
    };

    // The merge is only free if this stays true: shared_state<void> holds one of these
    // by value, and an empty member lands in the padding after flags_ rather than
    // growing the allocation. allocate()/deallocate() both pass sizeof() to PMR, so a
    // growth here would be a real cost on every void future.
    static_assert(std::is_empty_v<result_storage<void>>,
                  "result_storage<void> must stay empty");

}} // namespace actor_zeta::detail