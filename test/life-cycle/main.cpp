#define CATCH_CONFIG_MAIN // This tells Catch to provide a main() - only do this in one cpp file
#include <catch2/catch.hpp>

#include "classes.hpp"
#include <actor-zeta.hpp>

TEST_CASE("life-cycle") {
    std::unique_ptr<actor_zeta::pmr::memory_resource> resource = std::unique_ptr<actor_zeta::pmr::memory_resource>(actor_zeta::pmr::get_default_resource());
    {
        // singal actor
        {
            REQUIRE(test_handlers::ptr_0_counter == 0);
            auto actor = actor_zeta::spawn<test_handlers>(resource.get());
            actor_zeta::send(actor.get(), actor->address(), &test_handlers::ptr_0);
            actor->resume(10);
            REQUIRE(test_handlers::ptr_0_counter == 1);
            REQUIRE(test_handlers::ptr_1_counter == 0);
        }
        // singal supervisor
        {
            auto supervisor = actor_zeta::spawn<dummy_supervisor>(resource.get(), 1, 100);
            REQUIRE(dummy_supervisor::constructor_counter == 1);
            actor_zeta::send(supervisor.get(), supervisor->address(), &dummy_supervisor::create_storage);
            actor_zeta::send(supervisor.get(), supervisor->address(), &dummy_supervisor::create_test_handlers);
            REQUIRE(dummy_supervisor::enqueue_base_counter == 2);
            REQUIRE(dummy_supervisor::add_actor_impl_counter == 2);
            REQUIRE(test_handlers::init_counter == 2);
            REQUIRE(static_cast<dummy_supervisor*>(supervisor.get())->actors_count() == 2);
        }

    }
    REQUIRE(dummy_supervisor::destructor_counter == 1);
    resource.release(); // todo hack
}