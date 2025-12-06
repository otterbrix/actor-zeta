#pragma once

#include <actor-zeta.hpp>
#include <actor-zeta/dispatch.hpp>
#include <actor-zeta/future.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <actor-zeta/config.hpp>

template<typename... Args>
class coro_ping_pong_actor final : public actor_zeta::basic_actor<coro_ping_pong_actor<Args...>> {
    using base_type = actor_zeta::basic_actor<coro_ping_pong_actor<Args...>>;

    coro_ping_pong_actor* partner_;
    actor_zeta::scheduler::sharing_scheduler* scheduler_;

public:
    explicit coro_ping_pong_actor(std::pmr::memory_resource* resource, actor_zeta::scheduler::sharing_scheduler* sched = nullptr)
        : base_type(resource)
        , partner_(nullptr)
        , scheduler_(sched) {
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

    actor_zeta::unique_future<void> behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<coro_ping_pong_actor, &coro_ping_pong_actor::start>) {
            actor_zeta::dispatch(this, &coro_ping_pong_actor::start, msg);
        } else if (cmd == actor_zeta::msg_id<coro_ping_pong_actor, &coro_ping_pong_actor::ping>) {
            actor_zeta::dispatch(this, &coro_ping_pong_actor::ping, msg);
        } else if (cmd == actor_zeta::msg_id<coro_ping_pong_actor, &coro_ping_pong_actor::pong>) {
            actor_zeta::dispatch(this, &coro_ping_pong_actor::pong, msg);
        }
        return actor_zeta::make_ready_future_void(this->resource());
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &coro_ping_pong_actor::start,
        &coro_ping_pong_actor::ping,
        &coro_ping_pong_actor::pong
    >;
};