#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <thread>
#include <vector>

// =============================================================================
// What this file actually covers
// =============================================================================
//
// It used to say a bad_shutdown_actor case had been removed because
// shutdown_guard_t "automatically calls begin_shutdown() before base class
// destructor", making the race impossible to reproduce. That ordering was never
// real: shutdown_guard_ was declared FIRST among the members, so it was destroyed
// LAST -- after state_, mailbox_ and current_behavior_ were already gone. It ran
// after everything it claimed to protect, and ~cooperative_actor's own body
// already published `destroying` and waited. The guard has since been deleted.
//
// What does protect the teardown is that body: publish `destroying`, then
// wait_for_activity_to_drain() for the thread holding `running` and for any
// sender already past enqueue_impl's gate.
//
// So this is a stress test, not a proof, and it is aimed at the surviving
// question: an actor destroyed under load must not be resumed or enqueued after
// its members are gone. Run it under TSan and ASan for that to mean anything.
// =============================================================================

// =============================================================================
// Test Actor - Demonstrates automatic teardown under load
// =============================================================================

class good_shutdown_actor final : public actor_zeta::basic_actor<good_shutdown_actor> {
public:
    explicit good_shutdown_actor(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<good_shutdown_actor>(resource)
        , counter_(0) {
    }

    // No explicit destructor needed: ~cooperative_actor publishes `destroying` and
    // waits out the runner and any in-flight sender before the members go.
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

// =============================================================================
// Aggressive Shutdown Test - Verifies automatic teardown under load
// =============================================================================

TEST_CASE("Aggressive Shutdown Test: Automatic teardown under load") {
    // TEST OBJECTIVE:
    // Verify that proper use of begin_shutdown() prevents race condition
    //
    // EXPECTED RESULT WITH TSAN:
    // - NO data races detected
    // - All operations synchronized correctly

    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 1000);
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

            std::this_thread::sleep_for(std::chrono::milliseconds(1));

            // begin_shutdown() called in destructor → safe destruction
        }

        futures.clear();
    }

    scheduler->stop();

    // If we reach here without TSan errors, test passed!
    REQUIRE(true);
}

TEST_CASE("Stress Test: Concurrent actor creation/destruction") {
    // TEST OBJECTIVE:
    // Stress test with many actors being created and destroyed concurrently
    //
    // EXPECTED RESULT:
    // - No races with proper begin_shutdown() usage
    // - No crashes or memory leaks

    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(8, 1000);
    scheduler->start();

    std::atomic<int> completed{0};
    constexpr int NUM_THREADS = 4;
    constexpr int NUM_ACTORS = 48;  // Must be divisible by NUM_THREADS

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < NUM_ACTORS / NUM_THREADS; ++i) {
                {
                    auto actor = actor_zeta::spawn<good_shutdown_actor>(resource);

                    // Send a few messages
                    for (int j = 0; j < 10; ++j) {
                        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                                      &good_shutdown_actor::slow_task, j);
                        if (needs_sched) {
                            scheduler->enqueue(actor.get());
                        }
                    }

                    // Tiny sleep to ensure some messages are queued
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }

                completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    scheduler->stop();

    REQUIRE(completed.load() == NUM_ACTORS);
}