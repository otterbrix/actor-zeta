#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <cstdio>
#include <memory_resource>
#include <actor-zeta.hpp>

// Diagnostic: what does GCC pass to operator new?

class diagnostic_actor final : public actor_zeta::basic_actor<diagnostic_actor> {
public:
    explicit diagnostic_actor(std::pmr::memory_resource* res)
        : actor_zeta::basic_actor<diagnostic_actor>(res) {}

    // Method WITHOUT parameters
    actor_zeta::unique_future<int> no_params() {
        co_return 42;
    }

    // Method WITH one parameter
    actor_zeta::unique_future<int> one_param(int x) {
        co_return x * 2;
    }

    // Method WITH two parameters
    actor_zeta::unique_future<int> two_params(int x, int y) {
        co_return x + y;
    }

    // Method WITH move-only parameter
    actor_zeta::unique_future<int> move_param(std::unique_ptr<int>&& p) {
        co_return p ? *p : -1;
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<diagnostic_actor, &diagnostic_actor::no_params>) {
            actor_zeta::dispatch(this, &diagnostic_actor::no_params, msg);
        } else if (cmd == actor_zeta::msg_id<diagnostic_actor, &diagnostic_actor::one_param>) {
            actor_zeta::dispatch(this, &diagnostic_actor::one_param, msg);
        } else if (cmd == actor_zeta::msg_id<diagnostic_actor, &diagnostic_actor::two_params>) {
            actor_zeta::dispatch(this, &diagnostic_actor::two_params, msg);
        } else if (cmd == actor_zeta::msg_id<diagnostic_actor, &diagnostic_actor::move_param>) {
            actor_zeta::dispatch(this, &diagnostic_actor::move_param, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &diagnostic_actor::no_params,
        &diagnostic_actor::one_param,
        &diagnostic_actor::two_params,
        &diagnostic_actor::move_param
    >;
};

TEST_CASE("GCC args diagnostic - no params") {
    auto* mr = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<diagnostic_actor>(mr);

    printf("\n=== no_params() ===\n");
    auto fut = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                &diagnostic_actor::no_params);
    actor->resume(100);

    REQUIRE(fut.available());
    REQUIRE(std::move(fut).get() == 42);
}

TEST_CASE("GCC args diagnostic - one param") {
    auto* mr = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<diagnostic_actor>(mr);

    printf("\n=== one_param(10) ===\n");
    auto fut = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                &diagnostic_actor::one_param, 10);
    actor->resume(100);

    REQUIRE(fut.available());
    REQUIRE(std::move(fut).get() == 20);
}

TEST_CASE("GCC args diagnostic - two params") {
    auto* mr = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<diagnostic_actor>(mr);

    printf("\n=== two_params(3, 7) ===\n");
    auto fut = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                &diagnostic_actor::two_params, 3, 7);
    actor->resume(100);

    REQUIRE(fut.available());
    REQUIRE(std::move(fut).get() == 10);
}

TEST_CASE("GCC args diagnostic - move param") {
    auto* mr = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<diagnostic_actor>(mr);

    printf("\n=== move_param(unique_ptr) ===\n");
    auto fut = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                &diagnostic_actor::move_param, std::make_unique<int>(100));
    actor->resume(100);

    REQUIRE(fut.available());
    REQUIRE(std::move(fut).get() == 100);
}