#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/actor/dispatch.hpp>

#include <memory>
#include <memory_resource>
#include <thread>

using namespace actor_zeta;

// Cooperative actor whose method produces a unique_future<int>.
// Used both as a worker-thread producer (driven by a real sharing_scheduler) and as a
// manually-pumped actor (driven by run_until_complete).
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

// ============================================================================
// Test 1: honest cross-thread consumer poll — a real sharing_scheduler worker is
// the producer; the main thread polls is_ready() and takes the value via take_ready().
// No task<>/sync_wait: there is nothing to pump locally, the worker resolves the
// future cross-thread.
// ============================================================================
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

    // Consumer drive on the main thread: the worker thread resolves the future cross-thread;
    // run_until_complete pumps a plain yield until ready, then takes the value (no manual loop).
    int result = run_until_complete(future, [] { std::this_thread::yield(); });
    REQUIRE(result == 42);

    // Respect shutdown order: stop the scheduler BEFORE the actor is destroyed.
    sched->stop();
}

// ============================================================================
// Test 2: run_until_complete drives a future to completion via a manual pump.
// ============================================================================
TEST_CASE("run_until_complete drives a future via a manual pump") {
    auto* resource = std::pmr::get_default_resource();

    // No scheduler: this actor is pumped manually on the calling thread.
    auto actor = spawn<compute_actor>(resource);

    auto [needs_sched, future] = send(actor.get(), &compute_actor::doubler, 50);

    int result = run_until_complete(future, [&] { actor->resume(1); });
    REQUIRE(result == 100);
}
