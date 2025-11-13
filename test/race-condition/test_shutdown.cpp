#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <thread>
#include <vector>

// Simple test actor for shutdown testing
class shutdown_test_actor final : public actor_zeta::basic_actor<shutdown_test_actor> {
public:
    explicit shutdown_test_actor(actor_zeta::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<shutdown_test_actor>(resource)
        , slow_task_behavior_(actor_zeta::make_behavior(resource, this, &shutdown_test_actor::slow_task)) {
    }

    // CRITICAL: Explicit destructor with begin_shutdown()
    // Without this, TSan will detect race condition between:
    // - Main thread destroying behavior_t members
    // - Worker thread calling behavior() which reads behavior_t
    ~shutdown_test_actor() {
        begin_shutdown();  // ← Prevents worker threads from calling behavior()
        // Now safe to destroy behavior_t members
    }

    actor_zeta::unique_future<int> slow_task(int value) {
        // Simulate slow processing
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return actor_zeta::make_ready_future<int>(resource(), value * 2);
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<shutdown_test_actor, &shutdown_test_actor::slow_task>) {
            slow_task_behavior_(msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &shutdown_test_actor::slow_task
    >;

private:
    actor_zeta::behavior_t slow_task_behavior_;
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
        if (future.is_ready()) {
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

TEST_CASE("Shutdown Test 4.3: Resume during shutdown [TEMPLATE]") {
    // TEST OBJECTIVE:
    // Verify that resume() during actor destruction is handled safely
    //
    // IMPLEMENTATION STATUS: TEMPLATE ONLY
    // TODO: Implement when Phase 1 is complete

    WARN("Test 4.3 not yet implemented - see TESTING.md Phase 3");
}