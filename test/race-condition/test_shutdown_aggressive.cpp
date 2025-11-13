#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <thread>
#include <vector>

// =============================================================================
// Test Actor WITHOUT begin_shutdown() - reproduces race condition
// =============================================================================

class bad_shutdown_actor final : public actor_zeta::basic_actor<bad_shutdown_actor> {
public:
    explicit bad_shutdown_actor(actor_zeta::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<bad_shutdown_actor>(resource)
        , slow_task_behavior_(actor_zeta::make_behavior(resource, this, &bad_shutdown_actor::slow_task))
        , counter_(0) {
    }

    // NO explicit destructor → NO begin_shutdown() call → RACE CONDITION!
    // Default destructor will destroy behavior_t while worker thread may still use it

    actor_zeta::unique_future<int> slow_task(int value) {
        // Simulate slow processing to increase race window
        counter_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        return actor_zeta::make_ready_future<int>(resource(), value * 2);
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<bad_shutdown_actor, &bad_shutdown_actor::slow_task>) {
            slow_task_behavior_(msg);  // ← Worker thread reads behavior_t HERE
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &bad_shutdown_actor::slow_task
    >;

    template<typename R>
    actor_zeta::unique_future<R> enqueue_impl(actor_zeta::mailbox::message_ptr msg) {
        return enqueue_sync_impl<R>(std::move(msg), [this](auto* msg) { behavior(msg); });
    }

    size_t processed_count() const { return counter_.load(std::memory_order_acquire); }

private:
    actor_zeta::behavior_t slow_task_behavior_;  // ← Main thread destroys THIS
    std::atomic<size_t> counter_;
};

// =============================================================================
// Test Actor WITH begin_shutdown() - proper shutdown
// =============================================================================

class good_shutdown_actor final : public actor_zeta::basic_actor<good_shutdown_actor> {
public:
    explicit good_shutdown_actor(actor_zeta::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<good_shutdown_actor>(resource)
        , slow_task_behavior_(actor_zeta::make_behavior(resource, this, &good_shutdown_actor::slow_task))
        , counter_(0) {
    }

    // CORRECT: Explicit destructor with begin_shutdown()
    ~good_shutdown_actor() {
        begin_shutdown();  // ← Prevents worker threads from calling behavior()
        // Now safe to destroy behavior_t members
    }

    actor_zeta::unique_future<int> slow_task(int value) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        return actor_zeta::make_ready_future<int>(resource(), value * 2);
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<good_shutdown_actor, &good_shutdown_actor::slow_task>) {
            slow_task_behavior_(msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &good_shutdown_actor::slow_task
    >;

    template<typename R>
    actor_zeta::unique_future<R> enqueue_impl(actor_zeta::mailbox::message_ptr msg) {
        return enqueue_sync_impl<R>(std::move(msg), [this](auto* msg) { behavior(msg); });
    }

    size_t processed_count() const { return counter_.load(std::memory_order_acquire); }

private:
    actor_zeta::behavior_t slow_task_behavior_;
    std::atomic<size_t> counter_;
};

// =============================================================================
// Aggressive Test - Reproduces Race Condition Reliably
// =============================================================================

TEST_CASE("Aggressive Shutdown Test: WITHOUT begin_shutdown() [REPRODUCES BUG]") {
    // TEST OBJECTIVE:
    // Reliably reproduce the TSan race condition when actor is destroyed
    // without calling begin_shutdown()
    //
    // EXPECTED RESULT WITH TSAN:
    // - Data race detected between:
    //   * Main thread: destroying behavior_t member
    //   * Worker thread: calling behavior() which reads behavior_t
    //
    // THIS TEST SHOULD FAIL WITH TSAN!

    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 1000);
    scheduler->start();

    constexpr int NUM_ITERATIONS = 10;  // Multiple iterations to increase race probability
    std::atomic<int> races_triggered{0};

    for (int iter = 0; iter < NUM_ITERATIONS; ++iter) {
        std::vector<actor_zeta::unique_future<int>> futures;

        {
            auto actor = actor_zeta::spawn<bad_shutdown_actor>(resource);

            // Flood actor with messages to keep it busy
            constexpr int NUM_MESSAGES = 100;
            for (int i = 0; i < NUM_MESSAGES; ++i) {
                auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                              &bad_shutdown_actor::slow_task, i);

                if (future.needs_scheduling()) {
                    scheduler->enqueue(actor.get());
                }

                // Keep some futures to increase pending message count
                if (i % 10 == 0) {
                    futures.push_back(std::move(future));
                }
            }

            // CRITICAL: Very short sleep - actor will still be processing
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

            // RACE CONDITION WINDOW:
            // - Worker threads are still calling behavior()
            // - Main thread destroys actor → destroys behavior_t
            // → TSan should detect race!

            ++races_triggered;
        }

        // Clear futures (may be cancelled)
        futures.clear();
    }

    scheduler->stop();

    REQUIRE(races_triggered == NUM_ITERATIONS);
    // If TSan is enabled, this test should report a data race!
}

TEST_CASE("Aggressive Shutdown Test: WITH begin_shutdown() [SHOULD PASS]") {
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