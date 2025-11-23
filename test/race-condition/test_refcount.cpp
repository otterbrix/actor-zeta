#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/dispatch.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <thread>
#include <vector>

// Simple test actor for refcount testing
class refcount_test_actor final : public actor_zeta::basic_actor<refcount_test_actor> {
public:
    explicit refcount_test_actor(actor_zeta::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<refcount_test_actor>(resource) {
    }

    actor_zeta::unique_future<int> echo(int value) {
        return actor_zeta::make_ready_future<int>(resource(), value);
    }

    actor_zeta::unique_future<void> behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<refcount_test_actor, &refcount_test_actor::echo>) {
            return dispatch(this, &refcount_test_actor::echo, msg);
        }
        return actor_zeta::make_ready_future_void(resource());
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

    auto* resource = actor_zeta::pmr::get_default_resource();
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

TEST_CASE("Refcount Test 2.2: Stress test with 1000 concurrent futures [TEMPLATE]") {
    // TEST OBJECTIVE:
    // Stress test refcount management with many concurrent futures
    //
    // IMPLEMENTATION STATUS: TEMPLATE ONLY
    // TODO: Implement when Phase 1 is complete

    WARN("Test 2.2 not yet implemented - see TESTING.md Phase 2");
}

TEST_CASE("Refcount Test 2.3: Refcount underflow detection [TEMPLATE]") {
    // TEST OBJECTIVE:
    // Verify that refcount underflow is detected (debug builds)
    //
    // IMPLEMENTATION STATUS: TEMPLATE ONLY
    // TODO: Implement when Phase 1 is complete

    WARN("Test 2.3 not yet implemented - see TESTING.md Phase 3");
}