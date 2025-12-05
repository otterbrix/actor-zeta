#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/dispatch.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <thread>
#include <vector>

// Simple test actor for shutdown testing
class shutdown_test_actor final : public actor_zeta::basic_actor<shutdown_test_actor> {
public:
    explicit shutdown_test_actor(actor_zeta::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<shutdown_test_actor>(resource) {
    }

    // NOTE: No explicit destructor needed!
    // shutdown_guard_t automatically calls begin_shutdown() before base class destructor.
    // This prevents race condition between:
    // - Main thread destroying dispatch() members
    // - Worker thread calling behavior() which uses dispatch()
    // Default destructor = shutdown_guard_t protection + clean dispatch() destruction
    ~shutdown_test_actor() = default;

    actor_zeta::unique_future<int> slow_task(int value) {
        // Simulate slow processing
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return actor_zeta::make_ready_future<int>(resource(), value * 2);
    }

    actor_zeta::unique_future<void> behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<shutdown_test_actor, &shutdown_test_actor::slow_task>) {
            return dispatch(this, &shutdown_test_actor::slow_task, msg);
        }
        return actor_zeta::make_ready_future_void(resource());
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &shutdown_test_actor::slow_task
    >;
};

// =============================================================================
// Phase 1: Critical Tests - Actor Shutdown
// =============================================================================

TEST_CASE("Shutdown Test 4.1: Actor destroyed with pending futures") {
    // TEST OBJECTIVE:
    // Verify that actor can be safely destroyed while futures are still alive
    //
    // SCENARIO:
    // 1. Create actor and send messages
    // 2. Keep futures alive (don't call get())
    // 3. Destroy actor
    // 4. Verify: No crashes, no memory leaks
    //
    // EXPECTED BEHAVIOR:
    // - Futures should be marked as orphaned
    // - Messages still processed (or cancelled during shutdown)
    // - No assertion failures in actor destructor
    //
    // VERIFICATION:
    // - ASan will detect memory leaks
    // - Test completes without crashes = SUCCESS

    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);
    scheduler->start();

    // Store futures that outlive the actor
    std::vector<actor_zeta::unique_future<int>> futures;

    {
        // Actor scope - will be destroyed before futures
        auto actor = actor_zeta::spawn<shutdown_test_actor>(resource);

        // Send multiple messages and store futures
        constexpr int NUM_MESSAGES = 10;
        for (int i = 0; i < NUM_MESSAGES; ++i) {
            auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                          &shutdown_test_actor::slow_task, i);

            if (future.needs_scheduling()) {
                scheduler->enqueue(actor.get());
            }

            futures.push_back(std::move(future));
        }

        // Give some time for messages to be enqueued
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        // Actor destroyed here - while futures are still alive!
        // This should NOT crash or leak memory
    }

    // Futures still alive here - verify they handle orphaned state correctly
    // In debug builds, actor destructor may assert if pending_futures_count_ > 0
    // This test verifies the framework handles this scenario gracefully

    // Try to get results from futures (may be cancelled or error state)
    int successful = 0;
    for (auto& future : futures) {
        if (future.available()) {
            // Some messages may have been processed before actor destruction
            // Note: get() may return error state - we just count successful completions
            auto result = std::move(future).get();
            (void)result;
            ++successful;
        }
    }

    scheduler->stop();

    // Verification: Test completes without crashes
    // Number of successful futures may vary (0 to NUM_MESSAGES)
    REQUIRE(successful >= 0);  // Just verify we didn't crash

    // If we reach here without ASan errors, test passed!
}

TEST_CASE("Shutdown Test 4.2: Graceful shutdown - wait for all futures") {
    // TEST OBJECTIVE:
    // Verify graceful shutdown pattern where all futures are awaited
    //
    // SCENARIO:
    // 1. Create actor and send messages
    // 2. Wait for all futures to complete
    // 3. Destroy actor
    //
    // EXPECTED BEHAVIOR:
    // - All messages processed successfully
    // - Actor destroyed cleanly with no pending futures
    // - No assertion failures
    //
    // VERIFICATION:
    // - All futures return correct results
    // - No memory leaks (ASan)

    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);
    scheduler->start();

    std::vector<actor_zeta::unique_future<int>> futures;

    {
        auto actor = actor_zeta::spawn<shutdown_test_actor>(resource);

        // Send messages and store futures
        constexpr int NUM_MESSAGES = 10;
        for (int i = 0; i < NUM_MESSAGES; ++i) {
            auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                          &shutdown_test_actor::slow_task, i);

            if (future.needs_scheduling()) {
                scheduler->enqueue(actor.get());
            }

            futures.push_back(std::move(future));
        }

        // GRACEFUL SHUTDOWN: Wait for all futures before destroying actor
        int completed = 0;
        for (size_t i = 0; i < futures.size(); ++i) {
            auto result = std::move(futures[i]).get();
            REQUIRE(result == static_cast<int>(i) * 2);  // Verify correct result
            ++completed;
        }

        REQUIRE(completed == NUM_MESSAGES);

        // Actor destroyed here - all futures consumed, no pending messages
    }

    scheduler->stop();

    // Verification: All messages processed successfully
    // Test completes without crashes or assertions
}

// =============================================================================
// Template for Future Tests
// =============================================================================

TEST_CASE("Shutdown Test 4.3: Resume during shutdown") {
    // TEST OBJECTIVE:
    // Verify that resume() during actor destruction is handled safely.
    // This tests the race between:
    // - Main thread: destroying actor (calls begin_shutdown())
    // - Scheduler thread: calling resume() on the actor
    //
    // SCENARIO:
    // 1. Create actor and send many messages
    // 2. Start destroying actor while scheduler is still processing
    // 3. Scheduler may call resume() during/after begin_shutdown()
    //
    // EXPECTED BEHAVIOR:
    // - shutdown_guard_t prevents use-after-free of dispatch members
    // - Actor state check prevents processing after shutdown
    // - No crashes, no data races
    //
    // VERIFICATION:
    // - ASan detects use-after-free
    // - TSan detects data races
    // - Test completes without crashes = SUCCESS

    auto* resource = actor_zeta::pmr::get_default_resource();

    constexpr int NUM_ITERATIONS = 50;

    for (int iter = 0; iter < NUM_ITERATIONS; ++iter) {
        auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 1000);
        scheduler->start();

        {
            auto actor = actor_zeta::spawn<shutdown_test_actor>(resource);

            // Send burst of messages to create work backlog
            constexpr int NUM_MESSAGES = 100;
            std::vector<actor_zeta::unique_future<int>> futures;
            futures.reserve(NUM_MESSAGES);

            for (int i = 0; i < NUM_MESSAGES; ++i) {
                auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                              &shutdown_test_actor::slow_task, i);

                if (future.needs_scheduling()) {
                    scheduler->enqueue(actor.get());
                }

                futures.push_back(std::move(future));
            }

            // Random delay before destruction to vary race timing
            if (iter % 3 == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else if (iter % 3 == 1) {
                std::this_thread::yield();
            }
            // else: immediate destruction

            // Actor destroyed here while scheduler may be mid-resume()!
            // shutdown_guard_t should protect against use-after-free
        }

        scheduler->stop();
    }

    // If we reach here without crashes, test passed!
    REQUIRE(true);
}

TEST_CASE("Shutdown Test 4.4: Sequential create-destroy cycles") {
    // TEST OBJECTIVE:
    // Test actor lifecycle with sequential creation and destruction.
    // This catches shutdown race conditions in a controlled manner.
    //
    // SCENARIO:
    // 1. Create actor, send message, destroy actor
    // 2. Repeat sequentially
    // 3. Each cycle uses its own scheduler to avoid cross-cycle interference
    //
    // NOTE: This is a more conservative version that tests shutdown safety
    // without the additional complexity of concurrent actor creation.
    //
    // EXPECTED BEHAVIOR:
    // - No use-after-free (shutdown_guard_t protection)
    // - No memory leaks
    // - No crashes

    auto* resource = actor_zeta::pmr::get_default_resource();

    constexpr int NUM_CYCLES = 50;
    std::atomic<int> completed_cycles{0};

    for (int i = 0; i < NUM_CYCLES; ++i) {
        auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);
        scheduler->start();

        {
            // Create actor
            auto actor = actor_zeta::spawn<shutdown_test_actor>(resource);

            // Send message
            auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                          &shutdown_test_actor::slow_task, i);

            if (future.needs_scheduling()) {
                scheduler->enqueue(actor.get());
            }

            // Vary destruction timing
            switch (i % 4) {
                case 0: // Immediate
                    break;
                case 1: // Yield
                    std::this_thread::yield();
                    break;
                case 2: // Small sleep
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                    break;
                case 3: // Wait for result
                    if (future.available()) {
                        (void)std::move(future).get();
                    }
                    break;
            }

            // Actor destroyed here
        }

        scheduler->stop();
        completed_cycles.fetch_add(1, std::memory_order_relaxed);
    }

    REQUIRE(completed_cycles.load() == NUM_CYCLES);
}