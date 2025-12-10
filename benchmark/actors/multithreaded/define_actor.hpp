#pragma once

#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>

// Simple ping-pong actor for multithreaded benchmark
template<typename... Args>
class ping_pong_actor final : public actor_zeta::basic_actor<ping_pong_actor<Args...>> {
    using base_type = actor_zeta::basic_actor<ping_pong_actor<Args...>>;

    ping_pong_actor* partner_;
    actor_zeta::scheduler::sharing_scheduler* scheduler_;

public:
    explicit ping_pong_actor(std::pmr::memory_resource* resource, actor_zeta::scheduler::sharing_scheduler* sched = nullptr)
        : base_type(resource)
        , partner_(nullptr)
        , scheduler_(sched) {
    }

    ~ping_pong_actor() = default;

    void set_partner(ping_pong_actor* p) {
        partner_ = p;
    }

    void set_scheduler(actor_zeta::scheduler::sharing_scheduler* sched) {
        scheduler_ = sched;
    }

    actor_zeta::unique_future<void> start() {
        // Send ping to partner and schedule only if needed
        if (partner_ && scheduler_) {
            auto future = actor_zeta::send(partner_, this->address(), &ping_pong_actor::ping, Args{}...);
            // Only enqueue if actor was unblocked by this message
            if (future.needs_scheduling()) {
                scheduler_->enqueue(partner_);
            }
        }
        return actor_zeta::make_ready_future_void(this->resource());
    }

    actor_zeta::unique_future<void> ping(Args...) {
        // Receive ping, send pong back
        if (partner_ && scheduler_) {
            auto future = actor_zeta::send(partner_, this->address(), &ping_pong_actor::pong, Args{}...);
            // Only enqueue if actor was unblocked by this message
            if (future.needs_scheduling()) {
                scheduler_->enqueue(partner_);
            }
        }
        return actor_zeta::make_ready_future_void(this->resource());
    }

    actor_zeta::unique_future<void> pong(Args...) {
        // Receive pong, do nothing (end of exchange)
        return actor_zeta::make_ready_future_void(this->resource());
    }

    void behavior(actor_zeta::mailbox::message* msg) {

        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<ping_pong_actor, &ping_pong_actor::start>) {
            actor_zeta::dispatch(this, &ping_pong_actor::start, msg);
        } else if (cmd == actor_zeta::msg_id<ping_pong_actor, &ping_pong_actor::ping>) {
            actor_zeta::dispatch(this, &ping_pong_actor::ping, msg);
        } else if (cmd == actor_zeta::msg_id<ping_pong_actor, &ping_pong_actor::pong>) {
            actor_zeta::dispatch(this, &ping_pong_actor::pong, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &ping_pong_actor::start,
        &ping_pong_actor::ping,
        &ping_pong_actor::pong
    >;
};