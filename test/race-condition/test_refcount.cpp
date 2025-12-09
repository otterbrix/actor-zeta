#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// Helper: wait for future with timeout to prevent CI/CD hangs
template<typename T>
std::pair<bool, T> wait_with_timeout(actor_zeta::unique_future<T>&& future,
                                      std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();
    while (!future.available()) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > timeout) {
            return {false, T{}};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {true, std::move(future).get()};
}

// Wait for available() with timeout
template<typename T>
bool wait_available_with_timeout(actor_zeta::unique_future<T>& future,
                                  std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();
    while (!future.available()) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > timeout) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

constexpr auto FUTURE_TIMEOUT = std::chrono::seconds(10);

// Simple test actor for refcount testing
class refcount_test_actor final : public actor_zeta::basic_actor<refcount_test_actor> {
public:
    explicit refcount_test_actor(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<refcount_test_actor>(resource) {
    }

    actor_zeta::unique_future<int> echo(int value) {
        return actor_zeta::make_ready_future<int>(resource(), value);
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<refcount_test_actor, &refcount_test_actor::echo>) {
            dispatch(this, &refcount_test_actor::echo, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &refcount_test_actor::echo
    >;
};

// =============================================================================
// Phase 1: Critical Tests
// =============================================================================

TEST_CASE("Refcount Test 2.1: Concurrent actor + future release") {
    // TEST OBJECTIVE:
    // Verify that future_state refcount is correctly managed when:
    // - Actor processes message and calls slot->release() (refcount: 2 → 1)
    // - Future destructor calls slot->release() (refcount: 1 → 0 → delete)
    // Both operations may happen concurrently from different threads.
    //
    // EXPECTED BEHAVIOR:
    // - Slot is deleted exactly once
    // - No double-free
    // - No use-after-free
    // - Final refcount = 0
    //
    // VERIFICATION:
    // - ASan will detect double-free or use-after-free
    // - TSan will detect data races on refcount
    // - Test completes without crashes = SUCCESS

    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<refcount_test_actor>(resource);

    constexpr int NUM_ITERATIONS = 1000;
    std::atomic<int> completed{0};

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        // Create future - refcount starts at 2 (actor + future both hold references)
        auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                      &refcount_test_actor::echo, i);

        if (future.needs_scheduling()) {
            scheduler->enqueue(actor.get());
        }

        // Strategy: Destroy future in separate thread while actor processes message
        // This creates race between:
        // - Thread 1 (destroyer): future destructor calls slot->release()
        // - Thread 2 (scheduler): actor processes, calls slot->release()
        std::thread destroyer([fut = std::move(future)]() mutable {
            // Add small random delay to increase chance of race
            if (std::rand() % 2 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
            // Future destroyed here - calls slot->release()
            // May race with actor's slot->release() in message processing
        });

        destroyer.join();
        completed.fetch_add(1, std::memory_order_relaxed);
    }

    scheduler->stop();

    // Verification
    REQUIRE(completed.load() == NUM_ITERATIONS);
    // If we reach here without ASan/TSan errors, test passed!
    // ASan would catch: double-free, use-after-free
    // TSan would catch: data races on refcount operations
}

// =============================================================================
// Template for Future Tests
// =============================================================================

TEST_CASE("Refcount Test 2.2: Stress test with 1000 concurrent futures") {
    // TEST OBJECTIVE:
    // Stress test refcount management with many concurrent futures created
    // and destroyed simultaneously from multiple threads.
    //
    // SCENARIO:
    // 1. Create actor
    // 2. Launch N worker threads, each creating M futures concurrently
    // 3. Futures are destroyed with random timing
    // 4. Verify no memory corruption, no double-free, no use-after-free
    //
    // EXPECTED BEHAVIOR:
    // - All refcounts reach zero exactly once
    // - No data races on refcount operations
    // - No memory leaks
    //
    // VERIFICATION:
    // - ASan will detect memory errors
    // - TSan will detect data races

    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<refcount_test_actor>(resource);

    constexpr int NUM_THREADS = 4;
    constexpr int FUTURES_PER_THREAD = 250;  // Total: 1000 futures
    std::atomic<int> total_completed{0};
    std::atomic<bool> start_flag{false};

    std::vector<std::thread> workers;
    workers.reserve(NUM_THREADS);

    for (int t = 0; t < NUM_THREADS; ++t) {
        workers.emplace_back([&, thread_id = t]() {
            // Wait for all threads to be ready
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < FUTURES_PER_THREAD; ++i) {
                int value = thread_id * FUTURES_PER_THREAD + i;

                // Create future
                auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                              &refcount_test_actor::echo, value);

                if (future.needs_scheduling()) {
                    scheduler->enqueue(actor.get());
                }

                // Random destruction pattern to maximize race window
                int pattern = std::rand() % 4;
                switch (pattern) {
                    case 0:
                        // Immediate destruction
                        break;
                    case 1:
                        // Small delay
                        std::this_thread::sleep_for(std::chrono::microseconds(1));
                        break;
                    case 2:
                        // Yield to scheduler
                        std::this_thread::yield();
                        break;
                    case 3:
                        // Wait for ready then destroy
                        if (future.available()) {
                            auto result = std::move(future).get();
                            (void)result;
                        }
                        break;
                }
                // Future destroyed here (if not consumed)

                total_completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Start all threads simultaneously
    start_flag.store(true, std::memory_order_release);

    // Wait for all workers
    for (auto& worker : workers) {
        worker.join();
    }

    scheduler->stop();

    // Verification
    REQUIRE(total_completed.load() == NUM_THREADS * FUTURES_PER_THREAD);
    // If we reach here without ASan/TSan errors, test passed!
}

TEST_CASE("Refcount Test 2.3: Refcount correctness under various destruction patterns") {
    // TEST OBJECTIVE:
    // Verify that refcount is correctly managed in various destruction scenarios.
    // In debug builds, underflow would trigger assert in release().
    //
    // SCENARIOS TESTED:
    // 1. Future destroyed before actor processes (orphan)
    // 2. Future consumed via get() then destroyed
    // 3. Future destroyed after actor processes but before get()
    // 4. Multiple futures interleaved
    //
    // EXPECTED BEHAVIOR:
    // - Refcount reaches exactly 0 for each future_state
    // - No underflow (would trigger assert in debug builds)
    // - No memory leaks
    //
    // VERIFICATION:
    // - Debug builds: assert catches underflow
    // - ASan: detects memory errors
    // - Test completion without crash = SUCCESS

    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<refcount_test_actor>(resource);

    // Reduced iterations for TSan compatibility (TSan adds 10-50x overhead)
    // Must be divisible by 4 for Scenario 4 (4 threads × ITERATIONS/4)
    constexpr int ITERATIONS = 48;

    // Scenario 1: Orphan futures (destroyed before processing)
    SECTION("Scenario 1: Orphan futures") {
        for (int i = 0; i < ITERATIONS; ++i) {
            {
                auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                              &refcount_test_actor::echo, i);
                if (future.needs_scheduling()) {
                    scheduler->enqueue(actor.get());
                }
                // Future destroyed immediately - actor will process orphan message
            }
        }
        // Give time for actor to process orphan messages
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Scenario 2: Futures consumed via get()
    SECTION("Scenario 2: Consumed futures") {
        for (int i = 0; i < ITERATIONS; ++i) {
            auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                          &refcount_test_actor::echo, i);
            if (future.needs_scheduling()) {
                scheduler->enqueue(actor.get());
            }

            // Wait and consume with timeout
            auto [success, result] = wait_with_timeout(std::move(future), FUTURE_TIMEOUT);
            REQUIRE(success);
            REQUIRE(result == i);
        }
    }

    // Scenario 3: Check availability then destroy without consuming
    SECTION("Scenario 3: Ready but not consumed") {
        for (int i = 0; i < ITERATIONS; ++i) {
            auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                          &refcount_test_actor::echo, i);
            if (future.needs_scheduling()) {
                scheduler->enqueue(actor.get());
            }

            // Wait for ready with timeout
            bool ready = wait_available_with_timeout(future, FUTURE_TIMEOUT);
            REQUIRE(ready);

            // Destroy without calling get()
            // Refcount should still reach 0 correctly
        }
    }

    // Scenario 4: Interleaved patterns from multiple threads
    SECTION("Scenario 4: Interleaved multi-threaded") {
        std::atomic<int> completed{0};
        std::vector<std::thread> threads;

        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, pattern = t]() {
                for (int i = 0; i < ITERATIONS / 4; ++i) {
                    auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                                  &refcount_test_actor::echo, i);
                    if (future.needs_scheduling()) {
                        scheduler->enqueue(actor.get());
                    }

                    switch (pattern) {
                        case 0: // Immediate destroy
                            break;
                        case 1: // Consume
                            if (future.available()) {
                                (void)std::move(future).get();
                            }
                            break;
                        case 2: // Wait then destroy (with timeout)
                            (void)wait_available_with_timeout(future, FUTURE_TIMEOUT);
                            break;
                        case 3: // Random delay
                            std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 10));
                            break;
                    }
                    completed.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        REQUIRE(completed.load() == ITERATIONS);
    }

    scheduler->stop();

    // If we reach here without assert failures or crashes,
    // refcount management is correct (no underflow detected)
}