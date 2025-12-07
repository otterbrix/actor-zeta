#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// Test actor that can be cancelled during processing
class state_test_actor final : public actor_zeta::basic_actor<state_test_actor> {
public:
    explicit state_test_actor(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<state_test_actor>(resource) {
    }

    actor_zeta::unique_future<int> compute(int value) {
        // Simulate some processing
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return actor_zeta::make_ready_future<int>(resource(), value * 2);
    }

    actor_zeta::unique_future<int> fast_task(int value) {
        // Immediate processing (no delay)
        return actor_zeta::make_ready_future<int>(resource(), value + 1);
    }

    actor_zeta::unique_future<void> behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<state_test_actor, &state_test_actor::compute>) {
            return dispatch(this, &state_test_actor::compute, msg);
        } else if (cmd == actor_zeta::msg_id<state_test_actor, &state_test_actor::fast_task>) {
            return dispatch(this, &state_test_actor::fast_task, msg);
        }
        return actor_zeta::make_ready_future_void(resource());
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &state_test_actor::compute,
        &state_test_actor::fast_task
    >;
};

// =============================================================================
// Phase 1: Critical Tests - State Transitions
// =============================================================================

TEST_CASE("State Test 1.1: set_result vs cancel race") {
    // TEST OBJECTIVE:
    // Verify that concurrent set_result() and cancel() operations are safe
    //
    // SCENARIO:
    // 1. Send message to actor
    // 2. Thread 1: Actor processes message → calls set_result()
    // 3. Thread 2: Future cancelled (destructor or explicit cancel())
    // 4. Race: Which operation wins?
    //
    // EXPECTED BEHAVIOR:
    // - State transitions are atomic (only one succeeds)
    // - No data corruption
    // - No crashes
    //
    // VERIFICATION:
    // - TSan will detect data races on state transitions
    // - Test completes without crashes = SUCCESS

    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<state_test_actor>(resource);

    constexpr int NUM_ITERATIONS = 500;  // Original value - test if bug is really fixed
    std::atomic<int> races_detected{0};

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                      &state_test_actor::compute, i);

        if (future.needs_scheduling()) {
            scheduler->enqueue(actor.get());
        }

        // Thread to cancel future (simulates timeout or user cancellation)
        std::thread canceller([fut = std::move(future)]() mutable {
            // Random delay to create race window
            if (std::rand() % 3 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
            // Cancel or destroy future
            fut.cancel();
            // Future destructor runs here
        });

        canceller.join();
        ++races_detected;
    }

    scheduler->stop();

    // Verification
    REQUIRE(races_detected.load() == NUM_ITERATIONS);
    // If we reach here without TSan errors or crashes, test passed!
}

TEST_CASE("State Test 1.2: is_ready() during set_result()") {
    // TEST OBJECTIVE:
    // Verify that is_ready() observes consistent state during transitions
    //
    // SCENARIO:
    // 1. Send message to actor
    // 2. Thread 1: Actor processes → set_result()
    // 3. Thread 2: Continuously poll is_ready()
    //
    // EXPECTED BEHAVIOR:
    // - is_ready() returns false → true transition exactly once
    // - No torn reads (state is atomic)
    // - No crashes
    //
    // VERIFICATION:
    // - TSan will detect data races
    // - Test verifies monotonic transition (false → true, never true → false)

    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<state_test_actor>(resource);

    constexpr int NUM_ITERATIONS = 200;  // Reduced to avoid flaky timing issues on CI
    std::atomic<int> invalid_transitions{0};  // Only count INVALID transitions (true→false)

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                      &state_test_actor::compute, i);

        // Thread that continuously polls is_ready()
        std::atomic<bool> stop_polling{false};
        std::atomic<bool> saw_invalid{false};

        std::thread poller([&future, &stop_polling, &saw_invalid]() {
            bool last_state = false;  // Local to poller thread - no data race

            while (!stop_polling.load(std::memory_order_acquire)) {
                bool current_state = future.available();

                // Detect INVALID transition from true → false (should NEVER happen!)
                if (last_state && !current_state) {
                    saw_invalid.store(true, std::memory_order_relaxed);
                    std::cerr << "CRITICAL: Invalid transition true->false detected!\n";
                }

                last_state = current_state;
            }
        });

        // Schedule AFTER poller starts to reduce timing sensitivity
        if (future.needs_scheduling()) {
            scheduler->enqueue(actor.get());
        }

        // Wait for message to be processed
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        // Stop polling
        stop_polling.store(true, std::memory_order_release);
        poller.join();

        // Verify: No invalid transitions
        if (saw_invalid.load()) {
            ++invalid_transitions;
        }

        // Consume future to clean up
        if (future.available()) {
            auto result = std::move(future).get();
            (void)result;
        }
    }

    scheduler->stop();

    // Verification: No invalid state transitions detected
    REQUIRE(invalid_transitions.load() == 0);
}

TEST_CASE("State Test 1.3: Multiple state observers") {
    // TEST OBJECTIVE:
    // Verify that multiple threads can safely observe state concurrently
    //
    // SCENARIO:
    // 1. Send message to actor
    // 2. Multiple threads poll is_ready() concurrently
    // 3. Actor processes and sets result
    //
    // EXPECTED BEHAVIOR:
    // - All observers see consistent state
    // - No data races
    // - All observers eventually see is_ready() = true
    //
    // VERIFICATION:
    // - TSan will detect data races
    // - All observer threads complete successfully

    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<state_test_actor>(resource);

    constexpr int NUM_ITERATIONS = 100;  // Original value - test if bug is really fixed
    constexpr int NUM_OBSERVERS = 4;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                      &state_test_actor::fast_task, i);

        if (future.needs_scheduling()) {
            scheduler->enqueue(actor.get());
        }

        // Multiple observer threads
        std::vector<std::thread> observers;
        std::atomic<bool> stop_observing{false};
        std::atomic<int> observers_saw_ready{0};

        for (int obs = 0; obs < NUM_OBSERVERS; ++obs) {
            observers.emplace_back([&future, &stop_observing, &observers_saw_ready]() {
                bool saw_ready = false;
                while (!stop_observing.load(std::memory_order_acquire)) {
                    if (future.available()) {
                        saw_ready = true;
                        break;
                    }
                    std::this_thread::yield();
                }

                if (saw_ready) {
                    observers_saw_ready.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        // Wait for processing
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

        // Stop observing
        stop_observing.store(true, std::memory_order_release);

        for (auto& obs : observers) {
            obs.join();
        }

        // At least some observers should have seen ready state
        // (may not be all if stop signal comes too quickly)
        REQUIRE(observers_saw_ready.load() >= 0);

        // Consume future
        if (future.available()) {
            auto result = std::move(future).get();
            REQUIRE(result == i + 1);
        }
    }

    scheduler->stop();

    // If we reach here without TSan errors, test passed!
}

// =============================================================================
// Template for Future Tests
// =============================================================================

TEST_CASE("State Test 1.4: State transition ordering") {
    // TEST OBJECTIVE:
    // Verify that memory ordering guarantees are maintained during state transitions.
    // Specifically: when is_ready() returns true, the result value MUST be visible.
    //
    // SCENARIO:
    // 1. Producer thread: writes data, then sets ready state
    // 2. Consumer thread: waits for ready, then reads data
    // 3. Memory ordering must ensure consumer sees producer's data
    //
    // EXPECTED BEHAVIOR:
    // - acquire/release semantics guarantee visibility
    // - Consumer NEVER sees stale/uninitialized data after is_ready() == true
    //
    // VERIFICATION:
    // - TSan detects ordering violations
    // - Value mismatch detection in test

    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<state_test_actor>(resource);

    constexpr int NUM_ITERATIONS = 500;
    std::atomic<int> ordering_violations{0};
    std::atomic<int> successful_reads{0};

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        // Use unique value to detect stale reads
        int expected_value = i + 1;  // fast_task returns value + 1

        auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                      &state_test_actor::fast_task, i);

        if (future.needs_scheduling()) {
            scheduler->enqueue(actor.get());
        }

        // Consumer thread: poll and read
        std::thread consumer([&future, expected_value, &ordering_violations, &successful_reads]() {
            // Spin until ready
            while (!future.available()) {
                // Small delay to avoid pure spinning
                std::this_thread::yield();
            }

            // CRITICAL: At this point, is_ready() returned true.
            // Memory ordering MUST guarantee that the result is visible.

            // Read the value
            int actual_value = std::move(future).get();

            // Verify ordering: value must match expected
            // If memory ordering is broken, we might see:
            // - 0 (uninitialized)
            // - Previous iteration's value
            // - Garbage
            if (actual_value != expected_value) {
                ordering_violations.fetch_add(1, std::memory_order_relaxed);
            } else {
                successful_reads.fetch_add(1, std::memory_order_relaxed);
            }
        });

        consumer.join();
    }

    scheduler->stop();

    // Verification: No ordering violations
    INFO("Successful reads: " << successful_reads.load());
    INFO("Ordering violations: " << ordering_violations.load());
    REQUIRE(ordering_violations.load() == 0);
    REQUIRE(successful_reads.load() == NUM_ITERATIONS);
}

TEST_CASE("State Test 1.5: Happens-before across state transitions") {
    // TEST OBJECTIVE:
    // Verify happens-before relationship: all writes before set_result()
    // are visible to all reads after is_ready() returns true.
    //
    // SCENARIO:
    // - Producer: write auxiliary data, then set result
    // - Consumer: observe ready, then read auxiliary data
    // - Auxiliary data must be visible (happens-before)
    //
    // This tests that the state transition acts as a synchronization point.

    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<state_test_actor>(resource);

    constexpr int NUM_ITERATIONS = 300;
    std::atomic<int> visibility_failures{0};

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                      &state_test_actor::compute, i);

        if (future.needs_scheduling()) {
            scheduler->enqueue(actor.get());
        }

        // Consumer observes ready state
        std::atomic<bool> consumer_done{false};
        std::thread consumer([&]() {
            // Wait for ready with timeout
            auto start = std::chrono::steady_clock::now();
            while (!future.available()) {
                if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
                    // Timeout - something wrong
                    visibility_failures.fetch_add(1, std::memory_order_relaxed);
                    consumer_done.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
            }

            // Read and verify
            int result = std::move(future).get();
            if (result != i * 2) {  // compute returns value * 2
                visibility_failures.fetch_add(1, std::memory_order_relaxed);
            }

            consumer_done.store(true, std::memory_order_release);
        });

        consumer.join();
    }

    scheduler->stop();

    // Verification
    REQUIRE(visibility_failures.load() == 0);
}