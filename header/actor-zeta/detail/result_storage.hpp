#pragma once

#include <cassert>
#include <cstring>
#include <memory_resource>
#include <type_traits>
#include <utility>

namespace actor_zeta { namespace detail {

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

}} // namespace actor_zeta::detail