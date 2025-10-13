#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <actor-zeta/mailbox/priority.hpp>

namespace actor_zeta { namespace mailbox {

    namespace detail {
        static constexpr uint64_t answered_flag_mask = 0x4000000000000000;
        static constexpr uint64_t priority_flag_mask = 0x3000000000000000;
        static constexpr uint64_t high_message_priority = 0;
        static constexpr uint64_t normal_message_priority = 1;
        static constexpr uint64_t priority_offset = 60;
        static constexpr uint64_t default_async_value = 0x1000000000000000;
    } // namespace detail

    class message_id final {
    public:
        constexpr message_id()
            : value_(detail::default_async_value) {
        }

        constexpr explicit message_id(uint64_t value)
            : value_(value) {
        }

        message_id(const message_id&) = default;
        message_id& operator=(const message_id&) = default;

        constexpr bool operator==(const message_id& rhs) const noexcept {
            return value_ == rhs.value_;
        }

        constexpr bool operator<(const message_id& rhs) const noexcept {
            return value_ < rhs.value_;
        }

        constexpr uint64_t priority() const noexcept {
            return (value_ & detail::priority_flag_mask) >> detail::priority_offset;
        }

        constexpr message_id with_priority(uint64_t x) const noexcept {
            return message_id{(value_ & ~detail::priority_flag_mask) | (x << detail::priority_offset)};
        }

        constexpr bool is_answered() const noexcept {
            return (value_ & detail::answered_flag_mask) != 0;
        }

        constexpr bool is_high_message() const noexcept {
            return priority() == detail::high_message_priority;
        }

        constexpr bool is_normal_message() const noexcept {
            return priority() == detail::normal_message_priority;
        }

        constexpr message_id with_high_priority() const noexcept {
            return message_id{value_ & ~detail::priority_flag_mask};
        }

        constexpr message_id with_normal_priority() const noexcept {
            return message_id{value_ | detail::default_async_value};
        }

        constexpr uint64_t integer_value() const noexcept {
            return value_;
        }

        void mark_as_answered() noexcept {
            value_ |= detail::answered_flag_mask;
        }
        message_id& operator++() noexcept {
            ++value_;
            return *this;
        }

        constexpr operator uint64_t() const noexcept{
            return value_;
        }

    private:
        uint64_t value_;
    };

    constexpr message_id
    make_message_id(normal_priority_constant, uint64_t value) {
        return message_id{value | detail::default_async_value};
    }

    constexpr message_id
    make_message_id(high_priority_constant, uint64_t value) {
        return message_id{value};
    }

    template<priority P = priority::normal>
    constexpr message_id make_message_id(uint64_t value = 0) {
        return make_message_id(std::integral_constant<priority, P>{}, value);
    }

    constexpr message_id make_message_id(mailbox::priority p) {
        return message_id{static_cast<uint64_t>(p) << detail::priority_offset};
    }

}} // namespace actor_zeta::mailbox

namespace std {

    template<>
    struct hash<actor_zeta::mailbox::message_id> {
        size_t operator()(actor_zeta::mailbox::message_id x) const noexcept {
            hash<uint64_t> f;
            return f(x.integer_value());
        }
    };

} // namespace std
