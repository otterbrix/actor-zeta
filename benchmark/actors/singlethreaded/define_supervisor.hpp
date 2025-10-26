#pragma once

#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/scheduler.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <map>

template<typename Actor>
class simple_supervisor final : public actor_zeta::actor_abstract_t {
    actor_zeta::behavior_t prepare_behavior_;
    actor_zeta::behavior_t send_behavior_;

    std::unique_ptr<Actor, actor_zeta::pmr::deleter_t> actor_0_;
    std::unique_ptr<Actor, actor_zeta::pmr::deleter_t> actor_1_;
    actor_zeta::scheduler::sharing_scheduler* scheduler_;

public:
    explicit simple_supervisor(actor_zeta::pmr::memory_resource* ptr, actor_zeta::scheduler::sharing_scheduler* sched = nullptr)
        : actor_zeta::actor_abstract_t(ptr)
        , prepare_behavior_(actor_zeta::make_behavior(resource(), this, &simple_supervisor::prepare))
        , send_behavior_(actor_zeta::make_behavior(resource(), this, &simple_supervisor::send))
        , actor_0_(nullptr, actor_zeta::pmr::deleter_t(ptr))
        , actor_1_(nullptr, actor_zeta::pmr::deleter_t(ptr))
        , scheduler_(sched) {
    }

    void set_scheduler(actor_zeta::scheduler::sharing_scheduler* sched) {
        scheduler_ = sched;
    }

    void prepare() {
        // Spawn two actors
        actor_0_ = actor_zeta::spawn<Actor>(resource());
        actor_1_ = actor_zeta::spawn<Actor>(resource());

        // Set partners
        actor_0_->set_partner(actor_1_.get());
        actor_1_->set_partner(actor_0_.get());
    }

    void send() {
        // Start ping-pong - send start message to actor0
        if (actor_0_ && scheduler_) {
            actor_zeta::send(actor_0_.get(), this->address(), &Actor::start);
            scheduler_->enqueue(actor_0_.get());
        } else if (actor_0_) {
            // Synchronous mode (no scheduler)
            actor_zeta::send(actor_0_.get(), this->address(), &Actor::start);
            actor_0_->resume(1);
            actor_1_->resume(1);
            actor_0_->resume(1);
        }
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<simple_supervisor, &simple_supervisor::prepare>) {
            prepare_behavior_(msg);
        } else if (cmd == actor_zeta::msg_id<simple_supervisor, &simple_supervisor::send>) {
            send_behavior_(msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &simple_supervisor::prepare,
        &simple_supervisor::send
    >;

    /// @brief Override enqueue_impl для supervisor - используем helper
    template<typename R>
    unique_future<R> enqueue_impl(actor_zeta::mailbox::message_ptr msg) {
        return enqueue_sync_impl<R>(std::move(msg), [this](auto* ctx) { behavior(ctx); });
    }

protected:
};