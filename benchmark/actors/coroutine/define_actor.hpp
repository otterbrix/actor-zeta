#pragma once

#include <actor-zeta.hpp>
#include <actor-zeta/future.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <actor-zeta/config.hpp>

template<typename... Args>
class coro_ping_pong_actor final : public actor_zeta::basic_actor<coro_ping_pong_actor<Args...>> {
    using base_type = actor_zeta::basic_actor<coro_ping_pong_actor<Args...>>;

    coro_ping_pong_actor* partner_;
    actor_zeta::scheduler::sharing_scheduler* scheduler_;
    actor_zeta::behavior_t start_behavior_;
    actor_zeta::behavior_t ping_behavior_;
    actor_zeta::behavior_t pong_behavior_;

public:
    explicit coro_ping_pong_actor(actor_zeta::pmr::memory_resource* resource, actor_zeta::scheduler::sharing_scheduler* sched = nullptr)
        : base_type(resource)
        , partner_(nullptr)
        , scheduler_(sched)
        , start_behavior_(actor_zeta::make_behavior(resource, this, &coro_ping_pong_actor::start))
        , ping_behavior_(actor_zeta::make_behavior(resource, this, &coro_ping_pong_actor::ping))
        , pong_behavior_(actor_zeta::make_behavior(resource, this, &coro_ping_pong_actor::pong)) {
    }

    ~coro_ping_pong_actor() = default;

    void set_partner(coro_ping_pong_actor* p) {
        partner_ = p;
    }

    void set_scheduler(actor_zeta::scheduler::sharing_scheduler* sched) {
        scheduler_ = sched;
    }

    actor_zeta::unique_future<void> start() {
        if (partner_ && scheduler_) {
            auto future = actor_zeta::send(partner_, this->address(), &coro_ping_pong_actor::ping, Args{}...);
            if (future.needs_scheduling()) {
                scheduler_->enqueue(partner_);
            }
        }
        co_return;
    }

    actor_zeta::unique_future<void> ping(Args...) {
        if (partner_ && scheduler_) {
            auto future = actor_zeta::send(partner_, this->address(), &coro_ping_pong_actor::pong, Args{}...);
            if (future.needs_scheduling()) {
                scheduler_->enqueue(partner_);
            }
        }
        co_return;
    }

    actor_zeta::unique_future<void> pong(Args...) {
        co_return;
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<coro_ping_pong_actor, &coro_ping_pong_actor::start>) {
            start_behavior_(msg);
        } else if (cmd == actor_zeta::msg_id<coro_ping_pong_actor, &coro_ping_pong_actor::ping>) {
            ping_behavior_(msg);
        } else if (cmd == actor_zeta::msg_id<coro_ping_pong_actor, &coro_ping_pong_actor::pong>) {
            pong_behavior_(msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &coro_ping_pong_actor::start,
        &coro_ping_pong_actor::ping,
        &coro_ping_pong_actor::pong
    >;
};