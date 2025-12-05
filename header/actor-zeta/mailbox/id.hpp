#pragma once

#include <cstdint>

#include <actor-zeta/mailbox/priority.hpp>

namespace actor_zeta { namespace mailbox {

    namespace detail {
        static constexpr uint64_t high_message_priority = 0;
        static constexpr uint64_t normal_message_priority = 1;
    } // namespace detail

    /// @brief message_id is just a uint64_t
    using message_id = uint64_t;

    /// @brief Create message_id with normal priority (sets bit 60)
    constexpr message_id
    make_message_id(normal_priority_constant, uint64_t value) {
        return value | (detail::normal_message_priority << 60);
    }

    /// @brief Create message_id with high priority (bit 60 = 0)
    constexpr message_id
    make_message_id(high_priority_constant, uint64_t value) {
        return value;
    }

    /// @brief Extract priority from message_id (0 = high, 1 = normal)
    constexpr uint64_t message_priority(message_id id) noexcept {
        return (id >> 60) & 0x3;
    }

    /// @brief Create message_id with specified priority (default: normal)
    template<priority P = priority::normal>
    constexpr message_id make_message_id(uint64_t value = 0) {
        return make_message_id(std::integral_constant<priority, P>{}, value);
    }

}} // namespace actor_zeta::mailbox