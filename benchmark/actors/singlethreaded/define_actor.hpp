#pragma once

#include <actor-zeta.hpp>

#include <utility>

template<typename... Args>
class ping_pong_actor final : public actor_zeta::basic_actor<ping_pong_actor<Args...>> {
    using base_type = actor_zeta::basic_actor<ping_pong_actor<Args...>>;

    ping_pong_actor* partner_;
    // The partner's turn from send(): an actor cannot schedule its partner, so it records the
    // turn and the supervisor claims it. A plain bool: the supervisor drives both on one thread.
    bool partner_owed_ = false;

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
        if (partner_) {
            auto [needs_sched, future] = actor_zeta::send(partner_, &ping_pong_actor::ping, Args{}...);
            actor_zeta::detail::ignore_unused(future);
            partner_owed_ = partner_owed_ || needs_sched;
        }
        co_return;
    }

    actor_zeta::unique_future<void> ping(Args...) {
        if (partner_) {
            auto [needs_sched, future] = actor_zeta::send(partner_, &ping_pong_actor::pong, Args{}...);
            actor_zeta::detail::ignore_unused(future);
            partner_owed_ = partner_owed_ || needs_sched;
        }
        co_return;
    }

    actor_zeta::unique_future<void> pong(Args...) {
        co_return;
    }

    bool take_partner_obligation() noexcept {
        return std::exchange(partner_owed_, false);
    }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {

        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<ping_pong_actor, &ping_pong_actor::start>) {
            co_await actor_zeta::dispatch(this, &ping_pong_actor::start, msg);
        } else if (cmd == actor_zeta::msg_id<ping_pong_actor, &ping_pong_actor::ping>) {
            co_await actor_zeta::dispatch(this, &ping_pong_actor::ping, msg);
        } else if (cmd == actor_zeta::msg_id<ping_pong_actor, &ping_pong_actor::pong>) {
            co_await actor_zeta::dispatch(this, &ping_pong_actor::pong, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &ping_pong_actor::start,
        &ping_pong_actor::ping,
        &ping_pong_actor::pong
    >;
};