#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/dispatch.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <thread>
#include <vector>

// =============================================================================
// NOTE: bad_shutdown_actor test removed!
// =============================================================================
//
// REASON: With shutdown_guard_t, race condition is IMPOSSIBLE to reproduce!
//
// BEFORE (without shutdown_guard):
//   - Actor without explicit begin_shutdown() → race condition
//   - Test could reliably reproduce bug with TSan
//
// NOW (with shutdown_guard):
//   - shutdown_guard_t automatically calls begin_shutdown() for ALL actors
//   - Even actors without explicit destructor are SAFE
//   - Race condition cannot be reproduced anymore!
//
// CONCLUSION: This is GOOD! shutdown_guard_t provides automatic safety.
//             Test was useful for finding the bug, but now the bug is fixed.
// =============================================================================

// =============================================================================
// Test Actor - Demonstrates automatic shutdown_guard protection
// =============================================================================

class good_shutdown_actor final : public actor_zeta::basic_actor<good_shutdown_actor> {
public:
    explicit good_shutdown_actor(actor_zeta::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<good_shutdown_actor>(resource)
        , counter_(0) {
    }

    // NOTE: No explicit destructor needed!
    // shutdown_guard_t automatically calls begin_shutdown() before base class destructor.
    // This prevents race condition - safe to destroy dispatch() members.
    ~good_shutdown_actor() = default;

    actor_zeta::unique_future<int> slow_task(int value) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        return actor_zeta::make_ready_future<int>(resource(), value * 2);
    }

    actor_zeta::unique_future<void> behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<good_shutdown_actor, &good_shutdown_actor::slow_task>) {
            return dispatch(this, &good_shutdown_actor::slow_task, msg);
        }
        return actor_zeta::make_ready_future_void(resource());
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &good_shutdown_actor::slow_task
    >;

    template<typename R, typename... Args>
    actor_zeta::unique_future<R> enqueue_impl(
        actor_zeta::base::address_t sender,
        actor_zeta::mailbox::message_id cmd,
        Args&&... args
    ) {
        return enqueue_sync_impl<R>(
            sender,
            cmd,
            [this](auto* msg) { return behavior(msg); },
            std::forward<Args>(args)...
        );
    }

    size_t processed_count() const { return counter_.load(std::memory_order_acquire); }

private:
    std::atomic<size_t> counter_;
};

// =============================================================================
// Aggressive Shutdown Test - Verifies automatic shutdown_guard protection
// =============================================================================

TEST_CASE("Aggressive Shutdown Test: Automatic shutdown_guard protection") {
    // TEST OBJECTIVE:
    // Verify that proper use of begin_shutdown() prevents race condition
    //
    // EXPECTED RESULT WITH TSAN:
    // - NO data races detected
    // - All operations synchronized correctly

    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 1000);
    scheduler->start();

    constexpr int NUM_ITERATIONS = 10;

    for (int iter = 0; iter < NUM_ITERATIONS; ++iter) {
        std::vector<actor_zeta::unique_future<int>> futures;

        {
            auto actor = actor_zeta::spawn<good_shutdown_actor>(resource);

            constexpr int NUM_MESSAGES = 100;
            for (int i = 0; i < NUM_MESSAGES; ++i) {
                auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                              &good_shutdown_actor::slow_task, i);

                if (future.needs_scheduling()) {
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

    auto* resource = actor_zeta::pmr::get_default_resource();
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
                        auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                                      &good_shutdown_actor::slow_task, j);
                        if (future.needs_scheduling()) {
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