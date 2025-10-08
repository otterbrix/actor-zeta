#pragma once
#include <type_traits>

#include <actor-zeta/clock/clock.hpp>
#include <actor-zeta/base/address.hpp>

namespace actor_zeta { namespace clock {

    clock_t::~clock_t() {
    }

    clock_t::time_point clock_t::now() const noexcept {
        return clock_type::now();
    }

    void clock_t::schedule(time_point t, handler f) {
        return schedule_periodically(t, std::move(f), duration_type{0});
    }

    void clock_t::schedule_message(time_point t, base::address_t receiver, mailbox::message_ptr content) {
        schedule(
            t,
            handler(
                [receiver_tmp = std::move(receiver), content_tmp = std::move(content)]() mutable {
                    receiver_tmp->enqueue(std::move(content_tmp));
                }));
    }

    void clock_t::schedule(handler f) {
        return schedule_periodically(time_point{duration_type{0}}, std::move(f), duration_type{0});
    }

}} // namespace actor_zeta::clock
