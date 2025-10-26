#pragma once

#include <actor-zeta.hpp>

// Simple ping-pong actor for synchronous benchmark
template<typename... Args>
class ping_pong_actor final : public actor_zeta::basic_actor<ping_pong_actor<Args...>> {
    using base_type = actor_zeta::basic_actor<ping_pong_actor<Args...>>;

    ping_pong_actor* partner_;
    actor_zeta::behavior_t start_behavior_;
    actor_zeta::behavior_t ping_behavior_;
    actor_zeta::behavior_t pong_behavior_;

public:
    explicit ping_pong_actor(actor_zeta::pmr::memory_resource* resource)
        : base_type(resource)
        , partner_(nullptr)
        , start_behavior_(actor_zeta::make_behavior(resource, this, &ping_pong_actor::start))
        , ping_behavior_(actor_zeta::make_behavior(resource, this, &ping_pong_actor::ping))
        , pong_behavior_(actor_zeta::make_behavior(resource, this, &ping_pong_actor::pong)) {
    }

    ~ping_pong_actor() override = default;

    void set_partner(ping_pong_actor* p) {
        partner_ = p;
    }

    void start() {
        // Send ping to partner
        if (partner_) {
            actor_zeta::send(partner_, this->address(), &ping_pong_actor::ping, Args{}...);
        }
    }

    void ping(Args...) {
        // Receive ping, send pong back
        if (partner_) {
            actor_zeta::send(partner_, this->address(), &ping_pong_actor::pong, Args{}...);
        }
    }

    void pong(Args...) {
        // Receive pong, do nothing (end of exchange)
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<ping_pong_actor, &ping_pong_actor::start>) {
            start_behavior_(msg);
        } else if (cmd == actor_zeta::msg_id<ping_pong_actor, &ping_pong_actor::ping>) {
            ping_behavior_(msg);
        } else if (cmd == actor_zeta::msg_id<ping_pong_actor, &ping_pong_actor::pong>) {
            pong_behavior_(msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &ping_pong_actor::start,
        &ping_pong_actor::ping,
        &ping_pong_actor::pong
    >;
};