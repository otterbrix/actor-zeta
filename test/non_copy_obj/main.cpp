#define CATCH_CONFIG_MAIN // This tells Catch to provide a main() - only do this in one cpp file
#include <catch2/catch.hpp>

#include <cassert>

#include <map>
#include <memory>
#include <set>
#include <string>

#include "test/tooltestsuites/scheduler_test.hpp"
#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>

using std::pmr::memory_resource;
class dummy_supervisor;
class storage_t;

struct dummy_data {
    int number{0};
    std::string name{"default_name"};
};

// Verify decay_t<T&&> == decay_t<T> for RTT compatibility
static_assert(
    std::is_same_v<
        actor_zeta::type_traits::decay_t<std::unique_ptr<dummy_data>&&>,
        actor_zeta::type_traits::decay_t<std::unique_ptr<dummy_data>>
    >,
    "decay_t<T&&> must equal decay_t<T> for RTT compatibility"
);

class dummy_supervisor final : public actor_zeta::actor::actor_mixin<dummy_supervisor> {
public:
    template<typename T> using unique_future = actor_zeta::unique_future<T>;

    dummy_supervisor(memory_resource* ptr)
        : actor_zeta::actor::actor_mixin<dummy_supervisor>()
        , resource_(ptr)
        , executor_(new actor_zeta::test::scheduler_test_t(1, 1)) {
        executor_->start();
    }

    std::pmr::memory_resource* resource() const noexcept { return resource_; }

    auto scheduler_test() noexcept -> actor_zeta::test::scheduler_test_t* {
        return executor_.get();
    }

    // By-value for move-only types (GCC 11.4 bug workaround). See docs/GCC_COROUTINE_OPERATOR_NEW_BUG.md
    actor_zeta::unique_future<void> check(std::unique_ptr<dummy_data> data, dummy_data expected_data) {
        REQUIRE(data != nullptr);
        REQUIRE(data->number == expected_data.number);
        REQUIRE(data->name.size() == expected_data.name.size());
        REQUIRE(data->name == expected_data.name);
        co_return;
    }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        if (msg->command() == actor_zeta::msg_id<dummy_supervisor, &dummy_supervisor::check>) {
            co_await actor_zeta::dispatch(this, &dummy_supervisor::check, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &dummy_supervisor::check
    >;

private:
    std::pmr::memory_resource* resource_;
    std::unique_ptr<actor_zeta::test::scheduler_test_t> executor_;
    std::set<int64_t> ids_;
};

TEST_CASE("base move test") {
    auto* mr_ptr =std::pmr::get_default_resource();
    auto supervisor = actor_zeta::spawn<dummy_supervisor>(mr_ptr);

    auto ptr_data = std::unique_ptr<dummy_data>(new dummy_data);
    auto data = dummy_data();

    auto [needs_sched, fut] = actor_zeta::send(supervisor.get(), &dummy_supervisor::check, std::move(ptr_data), data);
    REQUIRE(ptr_data == nullptr);
    supervisor->scheduler_test()->run_once();
    std::move(fut).take_ready();
}

TEST_CASE("construct in place") {
    auto* mr_ptr =std::pmr::get_default_resource();
    auto supervisor = actor_zeta::spawn<dummy_supervisor>(mr_ptr);

    auto data = dummy_data();

    auto [needs_sched, fut] = actor_zeta::send(supervisor.get(), &dummy_supervisor::check, std::unique_ptr<dummy_data>(new dummy_data), data);
    supervisor->scheduler_test()->run_once();
    std::move(fut).take_ready();
}
