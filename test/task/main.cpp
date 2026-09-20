#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/actor/dispatch.hpp>
#include <test/tooltestsuites/scheduler_test.hpp>

#include <memory>
#include <memory_resource>
#include <thread>

using namespace actor_zeta;

// Cooperative actor whose method produces a unique_future<int>.
// Used both as a worker-thread producer (driven by a real sharing_scheduler) and as a
// manually-pumped actor (driven by a single-threaded scheduler_test_t).
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

    // Consumer drive on the main thread: the worker thread resolves the future
    // cross-thread, so there is nothing to pump locally — just poll is_ready() and
    // take the value once the producer has published it.
    constexpr int kAwaitCap = 10'000'000;
    for (int i = 0; i < kAwaitCap && !future.is_ready(); ++i) {
        std::this_thread::yield();
    }
    // is_ready() is promise_released, which a promise dying without a value also
    // sets; take_ready() only asserts has_result() and that assert is gone under
    // NDEBUG. The bound makes a stalled producer fail here instead of hanging.
    REQUIRE(future.is_ready());
    REQUIRE(!future.failed());
    int result = std::move(future).take_ready();
    REQUIRE(result == 42);

    // Respect shutdown order: stop the scheduler BEFORE the actor is destroyed.
    sched->stop();
}

// ============================================================================
// Test 2: pumping a future to completion by hand on the calling thread.
// ============================================================================
TEST_CASE("a manual pump drives a future to completion") {
    auto* resource = std::pmr::get_default_resource();

    // No worker threads: this actor is pumped manually on the calling thread through a
    // single-threaded scheduler_test_t, which consumes the resume verdict and re-queues
    // the job while it keeps asking to be resumed.
    auto actor = spawn<compute_actor>(resource);
    actor_zeta::test::scheduler_test_t sched(1, 100);

    auto [needs_sched, future] = send(actor.get(), &compute_actor::doubler, 50);
    sched.enqueue(actor.get());

    // Pump explicitly. The bound is the hang guard: a future whose producer is never
    // driven would otherwise spin here forever. And is_ready() is not a value gate --
    // it is the promise_released bit, which a promise dying without a value also sets,
    // while take_ready() only ASSERTS a value is present.
    constexpr int kPumpCap = 1'000'000;
    for (int i = 0; i < kPumpCap && !future.is_ready(); ++i) {
        sched.run_once();
    }
    REQUIRE(future.is_ready());
    REQUIRE_FALSE(future.failed());
    REQUIRE(std::move(future).take_ready() == 100);
}
