#define CATCH_CONFIG_MAIN // This tells Catch to provide a main() - only do this in one cpp file
#include <catch2/catch.hpp>

#include <cassert>

#include <map>
#include <memory>
#include <set>
#include <string>

#include "test/tooltestsuites/scheduler_test.hpp"
#include <actor-zeta.hpp>

using actor_zeta::pmr::memory_resource;
class dummy_supervisor;
class storage_t;

enum class command_t {
    check = 0x00
};

struct dummy_data {
    int number{0};
    std::string name{"default_name"};
};

class dummy_supervisor final : public actor_zeta::actor_abstract_t {
public:
    dummy_supervisor(memory_resource* ptr)
        : actor_abstract_t(ptr)
        , check_(actor_zeta::make_behavior(resource(), this, &dummy_supervisor::check))
        , executor_(new actor_zeta::test::scheduler_test_t(1, 1)) {
        executor_->start();
    }

    auto scheduler_test() noexcept -> actor_zeta::test::scheduler_test_t* {
        return executor_.get();
    }

    void check(std::unique_ptr<dummy_data>&& data, dummy_data expected_data);

    void behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<dummy_supervisor, &dummy_supervisor::check>: {
                check_(msg);
                break;
            }
        }
    }

    bool enqueue_impl(actor_zeta::message_ptr msg) override {
        auto tmp_msg = std::move(msg);
        behavior(tmp_msg.get());
        return true;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &dummy_supervisor::check
    >;

private:
    actor_zeta::behavior_t check_;
    std::unique_ptr<actor_zeta::test::scheduler_test_t> executor_;
    std::set<int64_t> ids_;
};

void dummy_supervisor::check(std::unique_ptr<dummy_data>&& data, dummy_data expected_data) {
    REQUIRE(data != nullptr);
    REQUIRE(data->number == expected_data.number);
    REQUIRE(data->name.size() == expected_data.name.size());
    REQUIRE(data->name == expected_data.name);
}

TEST_CASE("base move test") {
    auto* mr_ptr = actor_zeta::pmr::get_default_resource();
    auto supervisor = actor_zeta::spawn<dummy_supervisor>(mr_ptr);

    auto ptr_data = std::unique_ptr<dummy_data>(new dummy_data);
    auto data = dummy_data();

    actor_zeta::send(supervisor.get(), actor_zeta::address_t::empty_address(), &dummy_supervisor::check, std::move(ptr_data), data);
    REQUIRE(ptr_data == nullptr);
}

TEST_CASE("construct in place") {
    auto* mr_ptr = actor_zeta::pmr::get_default_resource();
    auto supervisor = actor_zeta::spawn<dummy_supervisor>(mr_ptr);

    auto data = dummy_data();

    actor_zeta::send(supervisor.get(), actor_zeta::address_t::empty_address(), &dummy_supervisor::check, std::unique_ptr<dummy_data>(new dummy_data), data);
}
