#pragma once

#include <actor-zeta.hpp>
#include <actor-zeta/dispatch.hpp>

// Simple ping-pong actor for synchronous benchmark
template<typename... Args>
class ping_pong_actor final : public actor_zeta::basic_actor<ping_pong_actor<Args...>> {
    using base_type = actor_zeta::basic_actor<ping_pong_actor<Args...>>;

    ping_pong_actor* partner_;

public:
    explicit ping_pong_actor(std::pmr::memory_resource* resource)
        : base_type(resource)
        , partner_(nullptr) {
    }

    ~ping_pong_actor() = default;

    void set_partner(ping_pong_actor* p) {
        partner_ = p;
    }

    actor_zeta::unique_future<void> start() {
        // Send ping to partner
        if (partner_) {
            actor_zeta::send(partner_, this->address(), &ping_pong_actor::ping, Args{}...);
        }
        return actor_zeta::make_ready_future_void(this->resource());
    }

    actor_zeta::unique_future<void> ping(Args...) {
        // Receive ping, send pong back
        if (partner_) {
            actor_zeta::send(partner_, this->address(), &ping_pong_actor::pong, Args{}...);
        }
        return actor_zeta::make_ready_future_void(this->resource());
    }

    actor_zeta::unique_future<void> pong(Args...) {
        // Receive pong, do nothing (end of exchange)
        return actor_zeta::make_ready_future_void(this->resource());
    }

    actor_zeta::unique_future<void> behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<ping_pong_actor, &ping_pong_actor::start>) {
            actor_zeta::dispatch(this, &ping_pong_actor::start, msg);
        } else if (cmd == actor_zeta::msg_id<ping_pong_actor, &ping_pong_actor::ping>) {
            actor_zeta::dispatch(this, &ping_pong_actor::ping, msg);
        } else if (cmd == actor_zeta::msg_id<ping_pong_actor, &ping_pong_actor::pong>) {
            actor_zeta::dispatch(this, &ping_pong_actor::pong, msg);
        }
        return actor_zeta::make_ready_future_void(this->resource());
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &ping_pong_actor::start,
        &ping_pong_actor::ping,
        &ping_pong_actor::pong
    >;
};