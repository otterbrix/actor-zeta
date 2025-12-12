#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>

// Good actor - uses void (fire-and-forget) and other types (request-response)
class good_actor final : public actor_zeta::basic_actor<good_actor> {
public:
    explicit good_actor(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<good_actor>(ptr) {
    }

    // All methods must be coroutines (use co_return)
    actor_zeta::unique_future<void> ping() {
        ping_count_++;
        co_return;
    }

    actor_zeta::unique_future<int> calculate() {
        co_return 42;
    }

    enum class status { ok, error };
    actor_zeta::unique_future<status> check_status() {
        co_return status::ok;
    }

    actor_zeta::unique_future<std::string> get_name() {
        co_return std::string("good_actor");
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<good_actor, &good_actor::ping>) {
            dispatch(this, &good_actor::ping, msg);
        } else if (cmd == actor_zeta::msg_id<good_actor, &good_actor::calculate>) {
            dispatch(this, &good_actor::calculate, msg);
        } else if (cmd == actor_zeta::msg_id<good_actor, &good_actor::check_status>) {
            dispatch(this, &good_actor::check_status, msg);
        } else if (cmd == actor_zeta::msg_id<good_actor, &good_actor::get_name>) {
            dispatch(this, &good_actor::get_name, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &good_actor::ping,
        &good_actor::calculate,
        &good_actor::check_status,
        &good_actor::get_name
    >;

    int get_ping_count() const { return ping_count_; }

private:
    int ping_count_ = 0;
};

TEST_CASE("void methods work (fire-and-forget)") {
    auto* resource =std::pmr::get_default_resource();

    auto actor = actor_zeta::spawn<good_actor>(resource);

    // Send void method - fire-and-forget
    auto future = actor_zeta::send(
        actor.get(),
        actor_zeta::address_t::empty_address(),
        &good_actor::ping);

    // Execute actor synchronously
    actor->resume(10);

    // Wait for completion
    std::move(future).get();

    REQUIRE(actor->get_ping_count() == 1);
}

TEST_CASE("int methods work (request-response)") {
    auto* resource =std::pmr::get_default_resource();

    auto actor = actor_zeta::spawn<good_actor>(resource);

    // Send int method - request-response
    auto future = actor_zeta::send(
        actor.get(),
        actor_zeta::address_t::empty_address(),
        &good_actor::calculate);

    // Execute actor synchronously
    actor->resume(10);

    // Wait for result
    int result = std::move(future).get();
    REQUIRE(result == 42);
}

TEST_CASE("enum methods work (request-response)") {
    auto* resource =std::pmr::get_default_resource();

    auto actor = actor_zeta::spawn<good_actor>(resource);

    // Send enum method - request-response
    auto future = actor_zeta::send(
        actor.get(),
        actor_zeta::address_t::empty_address(),
        &good_actor::check_status);

    // Execute actor synchronously
    actor->resume(10);

    // Wait for result
    auto status = std::move(future).get();
    REQUIRE(status == good_actor::status::ok);
}

TEST_CASE("string methods work (request-response)") {
    auto* resource =std::pmr::get_default_resource();

    auto actor = actor_zeta::spawn<good_actor>(resource);

    // Send string method - request-response
    auto future = actor_zeta::send(
        actor.get(),
        actor_zeta::address_t::empty_address(),
        &good_actor::get_name);

    // Execute actor synchronously
    actor->resume(10);

    // Wait for result
    std::string name = std::move(future).get();
    REQUIRE(name == "good_actor");
}

TEST_CASE("address_t works with all method types") {
    auto* resource =std::pmr::get_default_resource();

    auto actor = actor_zeta::spawn<good_actor>(resource);
    auto addr = actor->address();

    // void via address_t
    {
        auto future = actor_zeta::send(
            addr,
            actor_zeta::address_t::empty_address(),
            &good_actor::ping);
        actor->resume(10);
        std::move(future).get();
    }

    // int via address_t
    {
        auto future = actor_zeta::send(
            addr,
            actor_zeta::address_t::empty_address(),
            &good_actor::calculate);
        actor->resume(10);
        REQUIRE(std::move(future).get() == 42);
    }

    // enum via address_t
    {
        auto future = actor_zeta::send(
            addr,
            actor_zeta::address_t::empty_address(),
            &good_actor::check_status);
        actor->resume(10);
        REQUIRE(std::move(future).get() == good_actor::status::ok);
    }

    // string via address_t
    {
        auto future = actor_zeta::send(
            addr,
            actor_zeta::address_t::empty_address(),
            &good_actor::get_name);
        actor->resume(10);
        REQUIRE(std::move(future).get() == "good_actor");
    }
}

/*
// ❌ This code will NOT COMPILE - demonstrates that bool is prohibited
//
// Uncomment to verify static_assert triggers:

class bad_actor final : public actor_zeta::basic_actor<bad_actor> {
public:
    explicit bad_actor(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<bad_actor>(ptr) {}

    // ❌ PROHIBITED: returns bool
    bool check_something() {
        return true;
    }

    void behavior(actor_zeta::mailbox::message*) {}

    using dispatch_traits = actor_zeta::dispatch_traits<&bad_actor::check_something>;
};

TEST_CASE("bool methods are prohibited") {
    auto* resource =std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<bad_actor>(resource);

    // ❌ Compilation error: "Actor methods must not return bool"
    auto future = actor_zeta::send(
        actor.get(),
        actor_zeta::address_t::empty_address(),
        &bad_actor::check_something);
}
*/