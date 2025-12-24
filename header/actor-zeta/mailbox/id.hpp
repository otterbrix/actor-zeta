#pragma once

#include <cstdint>

#include <actor-zeta/mailbox/priority.hpp>

namespace actor_zeta { namespace mailbox {

    namespace detail {
        static constexpr uint64_t high_message_priority = 0;
        static constexpr uint64_t normal_message_priority = 1;
    } // namespace detail

    using message_id = uint64_t;

    constexpr message_id
    make_message_id(normal_priority_constant, uint64_t value) {
        return value | (detail::normal_message_priority << 60);
    }

    constexpr message_id
    make_message_id(high_priority_constant, uint64_t value) {
        return value;
    }

    constexpr uint64_t message_priority(message_id id) noexcept {
        return (id >> 60) & 0x3;
    }

    template<priority P = priority::normal>
    constexpr message_id make_message_id(uint64_t value = 0) {
        return make_message_id(std::integral_constant<priority, P>{}, value);
    }

}} // namespace actor_zeta::mailbox