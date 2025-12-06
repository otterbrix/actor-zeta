#pragma once

#include <actor-zeta.hpp>
#include <actor-zeta/dispatch.hpp>
#include <actor-zeta/scheduler/scheduler.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <map>

template<typename Actor>
class simple_supervisor final : public actor_zeta::base::actor_mixin<simple_supervisor<Actor>> {
    std::unique_ptr<Actor, actor_zeta::pmr::deleter_t> actor_0_;
    std::unique_ptr<Actor, actor_zeta::pmr::deleter_t> actor_1_;
    actor_zeta::scheduler::sharing_scheduler* scheduler_;
    std::pmr::memory_resource* resource_;

public:
    template<typename T>
    using unique_future = actor_zeta::unique_future<T>;

    explicit simple_supervisor(std::pmr::memory_resource* ptr, actor_zeta::scheduler::sharing_scheduler* sched = nullptr)
        : actor_zeta::base::actor_mixin<simple_supervisor<Actor>>()
        , actor_0_(nullptr, actor_zeta::pmr::deleter_t(ptr))
        , actor_1_(nullptr, actor_zeta::pmr::deleter_t(ptr))
        , scheduler_(sched)
        , resource_(ptr) {
    }

    std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    void set_scheduler(actor_zeta::scheduler::sharing_scheduler* sched) {
        scheduler_ = sched;
    }

    actor_zeta::unique_future<void> prepare() {
        // Spawn two actors with scheduler
        actor_0_ = actor_zeta::spawn<Actor>(resource_, scheduler_);
        actor_1_ = actor_zeta::spawn<Actor>(resource_, scheduler_);

        // Set partners
        actor_0_->set_partner(actor_1_.get());
        actor_1_->set_partner(actor_0_.get());
        return actor_zeta::make_ready_future_void(resource_);
    }

    actor_zeta::unique_future<void> send() {
        // Start ping-pong - send start message to actor0
        if (actor_0_ && scheduler_) {
            auto future = actor_zeta::send(actor_0_.get(), this->address(), &Actor::start);
            // Only enqueue if actor was unblocked by this message
            if (future.needs_scheduling()) {
                scheduler_->enqueue(actor_0_.get());
            }
        } else if (actor_0_) {
            // Synchronous mode (no scheduler)
            auto future = actor_zeta::send(actor_0_.get(), this->address(), &Actor::start);
            actor_0_->resume(1);
            actor_1_->resume(1);
            actor_0_->resume(1);
        }
        return actor_zeta::make_ready_future_void(resource_);
    }

    actor_zeta::unique_future<void> behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<simple_supervisor, &simple_supervisor::prepare>) {
            actor_zeta::dispatch(this, &simple_supervisor::prepare, msg);
        } else if (cmd == actor_zeta::msg_id<simple_supervisor, &simple_supervisor::send>) {
            actor_zeta::dispatch(this, &simple_supervisor::send, msg);
        }
        return actor_zeta::make_ready_future_void(resource_);
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &simple_supervisor::prepare,
        &simple_supervisor::send
    >;

    /// @brief Override enqueue_impl для supervisor - используем helper
    template<typename R, typename... Args>
    unique_future<R> enqueue_impl(
        actor_zeta::base::address_t sender,
        actor_zeta::mailbox::message_id cmd,
        Args&&... args
    ) {
        return this->template enqueue_sync_impl<R>(
            sender,
            cmd,
            [this](auto* ctx) { behavior(ctx); },
            std::forward<Args>(args)...
        );
    }

protected:
};