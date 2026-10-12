#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// Stress, not proof: teardown under load. delete publishes `destroying` and then
// waits out everyone inside the actor -- the puller in resume() and any sender
// already in enqueue_impl -- that much it does protect, and that is what these
// cases exercise.
//
// What it CANNOT protect is a job already sitting in the scheduler's queue: the
// actor holds no scheduler and cannot revoke it, so a worker picking that job up
// after the actor is gone reads freed memory. CLAUDE.md lists exactly that as
// unsafe. So each case here drains its own work before letting the actor die;
// destroying one mid-flight is UB, not a property to assert. ASan on Linux
// reported it; macOS ASan did not.

namespace {
    // Bounded: a stalled actor must fail the assertion, not hang the suite.
    template<typename Actor>
    [[nodiscard]] bool drain(Actor* actor, std::size_t expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (actor->processed_count() < expected) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    }
} // namespace

class good_shutdown_actor final : public actor_zeta::basic_actor<good_shutdown_actor> {
public:
    explicit good_shutdown_actor(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<good_shutdown_actor>(resource)
        , counter_(0) {
    }

    // ~cooperative_actor does the waiting; nothing to add here.
    ~good_shutdown_actor() = default;

    actor_zeta::unique_future<int> slow_task(int value) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        co_return value * 2;
    }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<good_shutdown_actor, &good_shutdown_actor::slow_task>) {
            co_await dispatch(this, &good_shutdown_actor::slow_task, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &good_shutdown_actor::slow_task
    >;

    size_t processed_count() const { return counter_.load(std::memory_order_acquire); }

private:
    std::atomic<size_t> counter_;
};

TEST_CASE("Aggressive Shutdown Test: Automatic teardown under load") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(resource, 4, 1000);
    scheduler->start();

    constexpr int NUM_ITERATIONS = 10;

    for (int iter = 0; iter < NUM_ITERATIONS; ++iter) {
        std::vector<actor_zeta::unique_future<int>> futures;

        {
            auto actor = actor_zeta::spawn<good_shutdown_actor>(resource);

            constexpr int NUM_MESSAGES = 100;
            for (int i = 0; i < NUM_MESSAGES; ++i) {
                auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                              &good_shutdown_actor::slow_task, i);

                if (needs_sched) {
                    scheduler->enqueue(actor.get());
                }

                if (i % 10 == 0) {
                    futures.push_back(std::move(future));
                }
            }

            // Drain before the actor dies: a queued job outliving it is UB.
            REQUIRE(drain(actor.get(), NUM_MESSAGES));
        }

        futures.clear();
    }

    scheduler->stop();
}

TEST_CASE("Stress Test: Concurrent actor creation/destruction") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(resource, 8, 1000);
    scheduler->start();

    std::atomic<int> completed{0};
    std::atomic<int> stranded{0};
    constexpr int NUM_THREADS = 4;
    constexpr int NUM_ACTORS = 48;  // Must be divisible by NUM_THREADS

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < NUM_ACTORS / NUM_THREADS; ++i) {
                {
                    auto actor = actor_zeta::spawn<good_shutdown_actor>(resource);

                    constexpr int kMessages = 10;
                    for (int j = 0; j < kMessages; ++j) {
                        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                                      &good_shutdown_actor::slow_task, j);
                        if (needs_sched) {
                            scheduler->enqueue(actor.get());
                        }
                    }

                    // Drained, not raced: destroying an actor whose job is still
                    // queued is UB. The concurrency under test is the spawn and
                    // teardown storm across threads, not that window.
                    if (!drain(actor.get(), kMessages)) {
                        stranded.fetch_add(1, std::memory_order_relaxed);
                    }
                }

                completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    scheduler->stop();

    REQUIRE(stranded.load() == 0);
    REQUIRE(completed.load() == NUM_ACTORS);
}