#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/actor/dispatch.hpp>
#include <test/tooltestsuites/scheduler_test.hpp>

#include <memory>
#include <memory_resource>
#include <thread>

using namespace actor_zeta;

// Used as a worker-thread producer (sharing_scheduler) and as a hand-pumped actor (scheduler_test_t).
class compute_actor final : public basic_actor<compute_actor> {
public:
    explicit compute_actor(std::pmr::memory_resource* res)
        : basic_actor<compute_actor>(res) {}

    unique_future<int> doubler(int x) {
        co_return x * 2;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &compute_actor::doubler
    >;

    behavior_t behavior(mailbox::message* msg) {
        if (msg->command() == msg_id<compute_actor, &compute_actor::doubler>) {
            co_await dispatch(this, &compute_actor::doubler, msg);
        }
    }
};

TEST_CASE("cross-thread: scheduler worker produces, main thread polls take_ready") {
    auto* resource = std::pmr::get_default_resource();

    std::unique_ptr<scheduler::sharing_scheduler> sched(
        new scheduler::sharing_scheduler(2, 100));
    sched->start();

    auto actor = spawn<compute_actor>(resource);

    auto [needs_sched, future] = send(actor.get(), &compute_actor::doubler, 21);
    if (needs_sched) {
        sched->enqueue(actor.get());
    }

    constexpr int kAwaitCap = 10'000'000;
    for (int i = 0; i < kAwaitCap && !future.is_ready(); ++i) {
        std::this_thread::yield(); // a worker resolves it cross-thread; nothing to pump locally
    }
    // is_ready() is promise_released, which a promise dying without a value also sets,
    // so failed() is gated separately. The bound makes a stalled producer fail, not hang.
    REQUIRE(future.is_ready());
    REQUIRE(!future.failed());
    int result = std::move(future).take_ready();
    REQUIRE(result == 42);

    sched->stop(); // BEFORE the actor is destroyed
}

TEST_CASE("a manual pump drives a future to completion") {
    auto* resource = std::pmr::get_default_resource();

    // No worker threads: pumped by hand on the calling thread.
    auto actor = spawn<compute_actor>(resource);
    actor_zeta::test::scheduler_test_t sched(1, 100);

    auto [needs_sched, future] = send(actor.get(), &compute_actor::doubler, 50);
    sched.enqueue(actor.get());

    // The bound is the hang guard for a never-driven producer; is_ready() is not a value gate, hence failed().
    constexpr int kPumpCap = 1'000'000;
    for (int i = 0; i < kPumpCap && !future.is_ready(); ++i) {
        sched.run_once();
    }
    REQUIRE(future.is_ready());
    REQUIRE_FALSE(future.failed());
    REQUIRE(std::move(future).take_ready() == 100);
}
