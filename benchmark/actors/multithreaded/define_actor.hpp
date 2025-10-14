#pragma once

#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>

// Simple ping-pong actor for multithreaded benchmark
template<typename... Args>
class ping_pong_actor final : public actor_zeta::basic_actor<ping_pong_actor<Args...>> {
    using base_type = actor_zeta::basic_actor<ping_pong_actor<Args...>>;

    ping_pong_actor* partner_;
    actor_zeta::scheduler::sharing_scheduler* scheduler_;
    actor_zeta::behavior_t start_behavior_;
    actor_zeta::behavior_t ping_behavior_;
    actor_zeta::behavior_t pong_behavior_;

public:
    explicit ping_pong_actor(actor_zeta::pmr::memory_resource* resource, actor_zeta::scheduler::sharing_scheduler* sched = nullptr)
        : base_type(resource)
        , partner_(nullptr)
        , scheduler_(sched)
        , start_behavior_(actor_zeta::make_behavior(resource, this, &ping_pong_actor::start))
        , ping_behavior_(actor_zeta::make_behavior(resource, this, &ping_pong_actor::ping))
        , pong_behavior_(actor_zeta::make_behavior(resource, this, &ping_pong_actor::pong)) {
    }

    ~ping_pong_actor() override = default;

    void set_partner(ping_pong_actor* p) {
        partner_ = p;
    }

    void set_scheduler(actor_zeta::scheduler::sharing_scheduler* sched) {
        scheduler_ = sched;
    }

    void start() {
        // Send ping to partner and schedule only if needed
        if (partner_ && scheduler_) {
            // send() returns true if actor was unblocked - needs scheduling
            if (actor_zeta::send(partner_, this->address(), &ping_pong_actor::ping, Args{}...)) {
                scheduler_->enqueue(partner_);
            }
        }
    }

    void ping(Args...) {
        // Receive ping, send pong back
        if (partner_ && scheduler_) {
            // send() returns true if actor was unblocked - needs scheduling
            if (actor_zeta::send(partner_, this->address(), &ping_pong_actor::pong, Args{}...)) {
                scheduler_->enqueue(partner_);
            }
        }
    }

    void pong(Args...) {
        // Receive pong, do nothing (end of exchange)
    }

    void behavior(actor_zeta::message* msg) {
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