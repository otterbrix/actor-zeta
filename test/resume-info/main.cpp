#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/resumable.hpp>

class test_actor final : public actor_zeta::basic_actor<test_actor> {
public:
    explicit test_actor(actor_zeta::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<test_actor>(ptr)
        , test_(actor_zeta::make_behavior(resource(), this, &test_actor::test)) {
    }

    void behavior(actor_zeta::message* msg) {
        if (msg->command() == actor_zeta::msg_id<test_actor, &test_actor::test>) {
            test_(msg);
        }
    }

    size_t processed_count() const { return processed_count_; }

    // Forward declaration for dispatch_traits
    void test();

    using dispatch_traits = actor_zeta::dispatch_traits<&test_actor::test>;

private:
    actor_zeta::behavior_t test_;
    size_t processed_count_ = 0;
};

void test_actor::test() {
    ++processed_count_;
}

TEST_CASE("resume_info - basic structure") {
    SECTION("Default constructor") {
        actor_zeta::scheduler::resume_info info;
        REQUIRE(info.result == actor_zeta::scheduler::resume_result::done);
        REQUIRE(info.messages_processed == 0);
    }

    SECTION("Parameterized constructor") {
        actor_zeta::scheduler::resume_info info(actor_zeta::scheduler::resume_result::resume, 42);
        REQUIRE(info.result == actor_zeta::scheduler::resume_result::resume);
        REQUIRE(info.messages_processed == 42);
    }

    SECTION("Implicit conversion to resume_result") {
        actor_zeta::scheduler::resume_info info(actor_zeta::scheduler::resume_result::awaiting, 10);
        actor_zeta::scheduler::resume_result result = info;
        REQUIRE(result == actor_zeta::scheduler::resume_result::awaiting);
    }
}

TEST_CASE("resume_info - actor returns correct message count") {
    auto* resource = actor_zeta::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<test_actor>(resource);

    SECTION("No messages - returns 0") {
        auto info = actor->resume(nullptr, 100);
        // When no messages, messages_processed should be 0
        REQUIRE(info.messages_processed == 0);
        // Result can be resume (try_block failed) or awaiting (try_block succeeded)
        // or done (inbox closed)
    }

    SECTION("Single message") {
        actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), &test_actor::test);

        auto info = actor->resume(nullptr, 100);
        REQUIRE(info.messages_processed == 1);
        REQUIRE(actor->processed_count() == 1);
    }

    SECTION("Multiple messages") {
        for (int i = 0; i < 5; ++i) {
            actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), &test_actor::test);
        }

        auto info = actor->resume(nullptr, 100);
        REQUIRE(info.messages_processed == 5);
        REQUIRE(actor->processed_count() == 5);
    }

    SECTION("Throughput limit respected") {
        for (int i = 0; i < 10; ++i) {
            actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), &test_actor::test);
        }

        auto info = actor->resume(nullptr, 3);
        REQUIRE(info.messages_processed == 3);
        REQUIRE(actor->processed_count() == 3);
        REQUIRE(info.result == actor_zeta::scheduler::resume_result::resume);

        // Resume again
        auto info2 = actor->resume(nullptr, 3);
        REQUIRE(info2.messages_processed == 3);
        REQUIRE(actor->processed_count() == 6);
    }
}

TEST_CASE("resume_info - backward compatibility with switch") {
    auto* resource = actor_zeta::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<test_actor>(resource);

    actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), &test_actor::test);

    auto info = actor->resume(nullptr, 100);

    // Should be able to switch on resume_info directly
    bool switched = false;
    switch (info) {
        case actor_zeta::scheduler::resume_result::resume:
            FAIL("Should not be resume");
            break;
        case actor_zeta::scheduler::resume_result::awaiting:
            switched = true;
            break;
        case actor_zeta::scheduler::resume_result::done:
            FAIL("Should not be done");
            break;
        case actor_zeta::scheduler::resume_result::shutdown:
            FAIL("Should not be shutdown");
            break;
    }

    REQUIRE(switched);
    REQUIRE(info.messages_processed == 1);
}