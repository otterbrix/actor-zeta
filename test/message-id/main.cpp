#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>

using actor_zeta::mailbox::make_message_id;
using actor_zeta::mailbox::message_id;
using actor_zeta::mailbox::message_priority;

TEST_CASE("make_message_id default") {
    auto x = make_message_id();
    REQUIRE(message_priority(x) == 1);  // normal priority
}

TEST_CASE("make_message_id with value") {
    auto x = make_message_id(42);
    REQUIRE(message_priority(x) == 1);  // normal priority
    REQUIRE((x & 0x0FFFFFFFFFFFFFFF) == 42);
}

TEST_CASE("make_message_id high priority") {
    auto x = make_message_id<actor_zeta::mailbox::priority::high>(42);
    REQUIRE(message_priority(x) == 0);  // high priority
}

TEST_CASE("make_message_id normal priority") {
    auto x = make_message_id<actor_zeta::mailbox::priority::normal>(42);
    REQUIRE(message_priority(x) == 1);  // normal priority
}

TEST_CASE("equality operator") {
    message_id a = 100;
    message_id b = 100;
    message_id c = 200;

    REQUIRE(a == b);
    REQUIRE(a != c);
}

TEST_CASE("message_id is uint64_t") {
    message_id x = 12345;
    uint64_t value = x;
    REQUIRE(value == 12345);
}