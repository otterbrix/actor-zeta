#define CATCH_CONFIG_MAIN // This tells Catch to provide a main() - only do this in one cpp file
#include <catch2/catch.hpp>

#include "classes.hpp"
#include <actor-zeta.hpp>

TEST_CASE("life-cycle") {
    std::unique_ptr<std::pmr::memory_resource> resource = std::unique_ptr<std::pmr::memory_resource>(std::pmr::get_default_resource());
    {
        // singal actor
        {
            REQUIRE(test_handlers::ptr_0_counter == 0);
            auto actor = actor_zeta::spawn<test_handlers>(resource.get());
            auto [needs_sched, fut] = actor_zeta::send(actor.get(), &test_handlers::ptr_0);
            actor->resume(10);
            std::move(fut).take_ready();
            REQUIRE(test_handlers::ptr_0_counter == 1);
            REQUIRE(test_handlers::ptr_1_counter == 0);
        }
        // singal supervisor
        {
            auto supervisor = actor_zeta::spawn<dummy_supervisor>(resource.get(), 1ULL, 100ULL);
            REQUIRE(dummy_supervisor::constructor_counter == 1);
            auto [needs_sched1, fut1] = actor_zeta::send(supervisor.get(), &dummy_supervisor::create_storage);
            auto [needs_sched2, fut2] = actor_zeta::send(supervisor.get(), &dummy_supervisor::create_test_handlers);
            std::move(fut1).take_ready();
            std::move(fut2).take_ready();
            REQUIRE(dummy_supervisor::enqueue_base_counter == 2);
            REQUIRE(dummy_supervisor::add_actor_impl_counter == 2);
            REQUIRE(test_handlers::init_counter == 2);
            REQUIRE(static_cast<dummy_supervisor*>(supervisor.get())->actors_count() == 2);
        }

    }
    REQUIRE(dummy_supervisor::destructor_counter == 1);
    resource.release(); // todo hack
}