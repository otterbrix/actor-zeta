#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>

class test_actor final : public actor_zeta::basic_actor<test_actor> {
public:
    explicit test_actor(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<test_actor>(ptr)
        , call_count1_(0)
        , call_count2_(0)
        , call_count3_(0) {
    }

    // Method definitions inside the class
    actor_zeta::unique_future<void> method1(int value) {
        ++call_count1_;
        last_value1_ = value;
        return actor_zeta::make_ready_future_void(resource());
    }

    actor_zeta::unique_future<void> method2(std::string text) {
        ++call_count2_;
        last_value2_ = text;
        return actor_zeta::make_ready_future_void(resource());
    }

    actor_zeta::unique_future<void> method3() {
        ++call_count3_;
        return actor_zeta::make_ready_future_void(resource());
    }

    // New dispatch_traits syntax - AFTER method definitions!
    using dispatch_traits = actor_zeta::dispatch_traits<
        &test_actor::method1,
        &test_actor::method2,
        &test_actor::method3
    >;

    actor_zeta::unique_future<void> behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<test_actor, &test_actor::method1>:
                return dispatch(this, &test_actor::method1, msg);
            case actor_zeta::msg_id<test_actor, &test_actor::method2>:
                return dispatch(this, &test_actor::method2, msg);
            case actor_zeta::msg_id<test_actor, &test_actor::method3>:
                return dispatch(this, &test_actor::method3, msg);
        }
        return actor_zeta::make_ready_future_void(resource());
    }

    int call_count1() const { return call_count1_; }
    int call_count2() const { return call_count2_; }
    int call_count3() const { return call_count3_; }
    int last_value1() const { return last_value1_; }
    std::string last_value2() const { return last_value2_; }

private:
    int call_count1_;
    int call_count2_;
    int call_count3_;
    int last_value1_;
    std::string last_value2_;
};

TEST_CASE("dispatch_traits - compile-time msg_id generation") {
    // Verify that msg_id is a compile-time constant
    constexpr auto id1 = actor_zeta::msg_id<test_actor, &test_actor::method1>;
    constexpr auto id2 = actor_zeta::msg_id<test_actor, &test_actor::method2>;
    constexpr auto id3 = actor_zeta::msg_id<test_actor, &test_actor::method3>;

    // message_id has default priority flag, so check that ActionId in lower bits is correct
    REQUIRE((id1 & 0xFFFFFFFF) == 0);
    REQUIRE((id2 & 0xFFFFFFFF) == 1);
    REQUIRE((id3 & 0xFFFFFFFF) == 2);
}

TEST_CASE("dispatch_traits - unique message IDs") {
    constexpr auto id1 = actor_zeta::msg_id<test_actor, &test_actor::method1>;
    constexpr auto id2 = actor_zeta::msg_id<test_actor, &test_actor::method2>;
    constexpr auto id3 = actor_zeta::msg_id<test_actor, &test_actor::method3>;

    REQUIRE(id1 != id2);
    REQUIRE(id2 != id3);
    REQUIRE(id1 != id3);
}

TEST_CASE("dispatch_traits - sequential indexing") {
    // ActionId should be sequential: 0, 1, 2, ...
    constexpr auto id1 = actor_zeta::msg_id<test_actor, &test_actor::method1>;
    constexpr auto id2 = actor_zeta::msg_id<test_actor, &test_actor::method2>;
    constexpr auto id3 = actor_zeta::msg_id<test_actor, &test_actor::method3>;

    // Extract ActionId from lower bits
    constexpr uint64_t action1 = id1 & 0xFFFFFFFF;
    constexpr uint64_t action2 = id2 & 0xFFFFFFFF;
    constexpr uint64_t action3 = id3 & 0xFFFFFFFF;

    REQUIRE(action1 == 0);
    REQUIRE(action2 == 1);
    REQUIRE(action3 == 2);
    REQUIRE(action2 == action1 + 1);
    REQUIRE(action3 == action2 + 1);
}

TEST_CASE("dispatch_traits - simple one-line syntax") {
    // This test verifies that the new syntax compiles
    // using dispatch_traits = actor_zeta::dispatch_traits<&Actor::method1, ...>;

    auto* resource =std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<test_actor>(resource);

    REQUIRE(actor != nullptr);
    REQUIRE(actor->call_count1() == 0);
    REQUIRE(actor->call_count2() == 0);
    REQUIRE(actor->call_count3() == 0);
}