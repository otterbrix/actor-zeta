/**
 * @file main.cpp
 * @brief Tests for code review fixes in future/promise system
 *
 * These tests verify the fixes for issues found during code review:
 * - Issue #1: wait_until_ready() infinite loop (CRITICAL)
 * - Issue #2: cancelled() no CAS (HIGH)
 * - Issue #3: void/non-void terminate asymmetry (HIGH)
 * - Issue #5: operator= overwrites error (MEDIUM-HIGH)
 * - Issue #6: error_code_ write before CAS (MEDIUM)
 *
 * See CODE_REVIEW_FINDINGS.md for details.
 */

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/detail/future.hpp>
#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/detail/intrusive_ptr.hpp>
#include <memory_resource>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace actor_zeta::detail;
using namespace actor_zeta;

// Helper: allocate future_state using PMR (matches destroy() deallocation)
template<typename T>
future_state<T>* allocate_future_state(std::pmr::memory_resource* resource) {
    void* mem = resource->allocate(sizeof(future_state<T>), alignof(future_state<T>));
    return new (mem) future_state<T>(resource);
}

// =============================================================================
// Issue #1: wait_until_ready() infinite loop fix
// BEFORE: while (!is_ready()) - loops forever on error/cancelled
// AFTER: while (!is_available()) - exits on any terminal state
// =============================================================================

TEST_CASE("Issue #1: wait_until_ready exits on error state", "[wait_until_ready][critical]") {
    auto* resource = std::pmr::get_default_resource();

    auto* state = allocate_future_state<int>(resource);
    intrusive_ptr<future_state_base> holder(state, false);

    // Set error state from another thread
    std::thread setter([state]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        state->error(std::make_error_code(std::errc::invalid_argument));
    });

    // wait_until_ready should exit when error is set (not loop forever)
    auto start = std::chrono::steady_clock::now();
    state->wait_until_ready();
    auto end = std::chrono::steady_clock::now();

    setter.join();

    // Should have exited quickly (within 1 second)
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    REQUIRE(duration.count() < 1000);

    // State should be error
    REQUIRE(state->is_failed());
    REQUIRE(state->is_error());
    REQUIRE(!state->is_ready());

    // is_available() should return true for error state
    REQUIRE(state->is_available());
}

TEST_CASE("Issue #1: wait_until_ready exits on cancelled state", "[wait_until_ready][critical]") {
    auto* resource = std::pmr::get_default_resource();

    auto* state = allocate_future_state<int>(resource);
    intrusive_ptr<future_state_base> holder(state, false);

    // Set cancelled state from another thread
    std::thread setter([state]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        state->cancelled();
    });

    // wait_until_ready should exit when cancelled (not loop forever)
    auto start = std::chrono::steady_clock::now();
    state->wait_until_ready();
    auto end = std::chrono::steady_clock::now();

    setter.join();

    // Should have exited quickly (within 1 second)
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    REQUIRE(duration.count() < 1000);

    // State should be cancelled
    REQUIRE(state->is_cancelled());
    REQUIRE(state->is_failed());

    // is_available() should return true for cancelled state
    REQUIRE(state->is_available());
}

TEST_CASE("Issue #1: wait_until_ready exits on ready state (normal case)", "[wait_until_ready]") {
    auto* resource = std::pmr::get_default_resource();

    auto* state = allocate_future_state<int>(resource);
    intrusive_ptr<future_state_base> holder(state, false);

    // Set value from another thread
    std::thread setter([state]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        state->set_value(42);
    });

    // wait_until_ready should exit when ready
    auto start = std::chrono::steady_clock::now();
    state->wait_until_ready();
    auto end = std::chrono::steady_clock::now();

    setter.join();

    // Should have exited quickly (within 1 second)
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    REQUIRE(duration.count() < 1000);

    // State should be ready with value
    REQUIRE(state->is_ready());
    REQUIRE(state->is_available());
    REQUIRE(state->get_value() == 42);
}

// =============================================================================
// Issue #2: cancelled() returns bool and uses CAS
// BEFORE: void cancelled() with direct store
// AFTER: bool cancelled() with CAS from pending state only
// =============================================================================

TEST_CASE("Issue #2: cancelled() returns true on success", "[cancelled][cas]") {
    auto* resource = std::pmr::get_default_resource();

    auto* state = allocate_future_state<int>(resource);
    intrusive_ptr<future_state_base> holder(state, false);

    REQUIRE(state->is_pending());

    // First cancel should succeed
    bool result = state->cancelled();
    REQUIRE(result == true);
    REQUIRE(state->is_cancelled());
}

TEST_CASE("Issue #2: cancelled() returns false if already cancelled", "[cancelled][cas]") {
    auto* resource = std::pmr::get_default_resource();

    auto* state = allocate_future_state<int>(resource);
    intrusive_ptr<future_state_base> holder(state, false);

    // First cancel
    bool first = state->cancelled();
    REQUIRE(first == true);

    // Second cancel should fail (already cancelled)
    bool second = state->cancelled();
    REQUIRE(second == false);
}

TEST_CASE("Issue #2: cancelled() returns false if already ready", "[cancelled][cas]") {
    auto* resource = std::pmr::get_default_resource();

    auto* state = allocate_future_state<int>(resource);
    intrusive_ptr<future_state_base> holder(state, false);

    // Set value first
    state->set_value(42);
    REQUIRE(state->is_ready());

    // Cancel should fail (already ready)
    bool result = state->cancelled();
    REQUIRE(result == false);
    REQUIRE(state->is_ready()); // Still ready, not cancelled
}

TEST_CASE("Issue #2: cancelled() returns false if already in error", "[cancelled][cas]") {
    auto* resource = std::pmr::get_default_resource();

    auto* state = allocate_future_state<int>(resource);
    intrusive_ptr<future_state_base> holder(state, false);

    // Set error first
    state->error(std::make_error_code(std::errc::invalid_argument));
    REQUIRE(state->is_error());

    // Cancel should fail (already in error)
    bool result = state->cancelled();
    REQUIRE(result == false);
    REQUIRE(state->is_error()); // Still error, not cancelled
}

TEST_CASE("Issue #2: cancelled() is atomic under contention", "[cancelled][cas][threading]") {
    auto* resource = std::pmr::get_default_resource();

    constexpr int NUM_ITERATIONS = 100;
    std::atomic<int> success_count{0};

    for (int iter = 0; iter < NUM_ITERATIONS; ++iter) {
        auto* state = allocate_future_state<int>(resource);
        intrusive_ptr<future_state_base> holder(state, false);

        std::atomic<int> local_success{0};
        constexpr int NUM_THREADS = 4;

        // Multiple threads try to cancel simultaneously
        std::vector<std::thread> threads;
        for (int t = 0; t < NUM_THREADS; ++t) {
            threads.emplace_back([state, &local_success]() {
                if (state->cancelled()) {
                    local_success.fetch_add(1);
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        // Exactly one thread should succeed
        REQUIRE(local_success.load() == 1);
        REQUIRE(state->is_cancelled());

        success_count.fetch_add(local_success.load());
    }

    // Total successes = NUM_ITERATIONS (one per iteration)
    REQUIRE(success_count.load() == NUM_ITERATIONS);
}

// =============================================================================
// Issue #3: void/non-void terminate asymmetry
// BEFORE: Different handling for void vs non-void in wait_for_ready()
// AFTER: Consistent std::terminate() for both via is_failed() check
// =============================================================================

TEST_CASE("Issue #3: error state handling is consistent for int type", "[terminate][consistency]") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p(resource);
    p.error(std::make_error_code(std::errc::invalid_argument));
    unique_future<int> f = p.get_future();

    // Future should report failed state
    REQUIRE(f.failed());
    REQUIRE(f.error() == std::make_error_code(std::errc::invalid_argument));

    // Note: We can't test std::terminate() directly, but we verify the state
}

TEST_CASE("Issue #3: error state handling is consistent for void type", "[terminate][consistency]") {
    auto* resource = std::pmr::get_default_resource();

    promise<void> p(resource);
    p.error(std::make_error_code(std::errc::invalid_argument));
    unique_future<void> f = p.get_future();

    // Future should report failed state
    REQUIRE(f.failed());
    REQUIRE(f.error() == std::make_error_code(std::errc::invalid_argument));

    // Note: We can't test std::terminate() directly, but we verify the state
}

TEST_CASE("Issue #3: cancelled state handling is consistent for int type", "[terminate][consistency]") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p(resource);
    unique_future<int> f = p.get_future();
    f.cancel();

    // Future should report cancelled state
    REQUIRE(f.is_cancelled());
    REQUIRE(f.failed());
    REQUIRE(f.error() == std::make_error_code(std::errc::operation_canceled));
}

TEST_CASE("Issue #3: cancelled state handling is consistent for void type", "[terminate][consistency]") {
    auto* resource = std::pmr::get_default_resource();

    promise<void> p(resource);
    unique_future<void> f = p.get_future();
    f.cancel();

    // Future should report cancelled state
    REQUIRE(f.is_cancelled());
    REQUIRE(f.failed());
    REQUIRE(f.error() == std::make_error_code(std::errc::operation_canceled));
}

// =============================================================================
// Issue #5: operator= overwrites error
// BEFORE: !is_ready() would overwrite error/cancelled states
// AFTER: is_pending() only cancels truly pending futures
// =============================================================================

TEST_CASE("Issue #5: operator= does not overwrite error state", "[operator=][error]") {
    auto* resource = std::pmr::get_default_resource();

    // Create first future with error
    promise<int> p1(resource);
    p1.error(std::make_error_code(std::errc::invalid_argument));
    unique_future<int> f1 = p1.get_future();

    // Create second future
    promise<int> p2(resource);
    p2.set_value(42);
    unique_future<int> f2 = p2.get_future();

    // Move f2 into f1 - should NOT overwrite f1's error state
    // f1 is already in error state, should not call cancelled()
    f1 = std::move(f2);

    // f1 now holds f2's state (which has value 42)
    REQUIRE(f1.available());
    REQUIRE(std::move(f1).get() == 42);
}

TEST_CASE("Issue #5: operator= does not overwrite cancelled state", "[operator=][cancelled]") {
    auto* resource = std::pmr::get_default_resource();

    // Create first future and cancel it
    promise<int> p1(resource);
    unique_future<int> f1 = p1.get_future();
    f1.cancel();
    REQUIRE(f1.is_cancelled());

    // Create second future
    promise<int> p2(resource);
    p2.set_value(42);
    unique_future<int> f2 = p2.get_future();

    // Move f2 into f1 - should NOT try to cancel f1 again
    // f1 is already cancelled, is_pending() returns false
    f1 = std::move(f2);

    // f1 now holds f2's state (which has value 42)
    REQUIRE(f1.available());
    REQUIRE(std::move(f1).get() == 42);
}

TEST_CASE("Issue #5: operator= cancels pending future", "[operator=][pending]") {
    auto* resource = std::pmr::get_default_resource();

    // Create first future (pending)
    promise<int> p1(resource);
    unique_future<int> f1 = p1.get_future();
    REQUIRE(!f1.available()); // Still pending

    // Create second future
    promise<int> p2(resource);
    p2.set_value(42);
    unique_future<int> f2 = p2.get_future();

    // Move f2 into f1 - should cancel f1 (which is pending)
    f1 = std::move(f2);

    // f1 now holds f2's state (which has value 42)
    REQUIRE(f1.available());
    REQUIRE(std::move(f1).get() == 42);

    // p1's state should now be cancelled
    // (Can't verify directly, but the operation should not crash)
}

// =============================================================================
// Issue #6: error_code_ write after CAS
// BEFORE: error_code_ = ec; then CAS (race window)
// AFTER: CAS first, then error_code_ = ec inside success block
// =============================================================================

TEST_CASE("Issue #6: error() writes error_code_ only after successful CAS", "[error][cas]") {
    auto* resource = std::pmr::get_default_resource();

    auto* state = allocate_future_state<int>(resource);
    intrusive_ptr<future_state_base> holder(state, false);

    // Set error
    bool success = state->error(std::make_error_code(std::errc::invalid_argument));
    REQUIRE(success);
    REQUIRE(state->is_error());
    REQUIRE(state->error() == std::make_error_code(std::errc::invalid_argument));
}

TEST_CASE("Issue #6: cancelled() writes error_code_ only after successful CAS", "[cancelled][cas]") {
    auto* resource = std::pmr::get_default_resource();

    auto* state = allocate_future_state<int>(resource);
    intrusive_ptr<future_state_base> holder(state, false);

    // Cancel
    bool success = state->cancelled();
    REQUIRE(success);
    REQUIRE(state->is_cancelled());
    REQUIRE(state->error() == std::make_error_code(std::errc::operation_canceled));
}

TEST_CASE("Issue #6: concurrent error/cancelled race - error_code_ consistent", "[error][cancelled][race]") {
    auto* resource = std::pmr::get_default_resource();

    constexpr int NUM_ITERATIONS = 100;

    for (int iter = 0; iter < NUM_ITERATIONS; ++iter) {
        auto* state = allocate_future_state<int>(resource);
        intrusive_ptr<future_state_base> holder(state, false);

        std::atomic<int> error_success{0};
        std::atomic<int> cancel_success{0};

        // Two threads race: one sets error, one cancels
        std::thread error_thread([state, &error_success]() {
            if (state->error(std::make_error_code(std::errc::invalid_argument))) {
                error_success.fetch_add(1);
            }
        });

        std::thread cancel_thread([state, &cancel_success]() {
            if (state->cancelled()) {
                cancel_success.fetch_add(1);
            }
        });

        error_thread.join();
        cancel_thread.join();

        // Exactly one should succeed
        REQUIRE(error_success.load() + cancel_success.load() == 1);

        // Verify error_code_ is consistent with state
        if (state->is_error()) {
            REQUIRE(state->error() == std::make_error_code(std::errc::invalid_argument));
        } else {
            REQUIRE(state->is_cancelled());
            REQUIRE(state->error() == std::make_error_code(std::errc::operation_canceled));
        }
    }
}

// =============================================================================
// Additional regression tests for stability
// =============================================================================

TEST_CASE("Regression: is_available() returns true for all terminal states", "[is_available][regression]") {
    auto* resource = std::pmr::get_default_resource();

    SECTION("ready state") {
        auto* state = allocate_future_state<int>(resource);
        intrusive_ptr<future_state_base> holder(state, false);

        state->set_value(42);
        REQUIRE(state->is_available());
        REQUIRE(state->is_ready());
    }

    SECTION("error state") {
        auto* state = allocate_future_state<int>(resource);
        intrusive_ptr<future_state_base> holder(state, false);

        state->error(std::make_error_code(std::errc::invalid_argument));
        REQUIRE(state->is_available());
        REQUIRE(state->is_error());
    }

    SECTION("cancelled state") {
        auto* state = allocate_future_state<int>(resource);
        intrusive_ptr<future_state_base> holder(state, false);

        state->cancelled();
        REQUIRE(state->is_available());
        REQUIRE(state->is_cancelled());
    }

    SECTION("consumed state") {
        auto* state = allocate_future_state<int>(resource);
        intrusive_ptr<future_state_base> holder(state, false);

        state->set_value(42);
        int value = state->take_value();
        REQUIRE(value == 42);
        REQUIRE(state->is_available());
        REQUIRE(state->is_consumed());
    }
}

TEST_CASE("Regression: is_pending() returns false for all terminal states", "[is_pending][regression]") {
    auto* resource = std::pmr::get_default_resource();

    SECTION("ready state") {
        auto* state = allocate_future_state<int>(resource);
        intrusive_ptr<future_state_base> holder(state, false);

        state->set_value(42);
        REQUIRE(!state->is_pending());
    }

    SECTION("error state") {
        auto* state = allocate_future_state<int>(resource);
        intrusive_ptr<future_state_base> holder(state, false);

        state->error(std::make_error_code(std::errc::invalid_argument));
        REQUIRE(!state->is_pending());
    }

    SECTION("cancelled state") {
        auto* state = allocate_future_state<int>(resource);
        intrusive_ptr<future_state_base> holder(state, false);

        state->cancelled();
        REQUIRE(!state->is_pending());
    }

    SECTION("consumed state") {
        auto* state = allocate_future_state<int>(resource);
        intrusive_ptr<future_state_base> holder(state, false);

        state->set_value(42);
        (void)state->take_value();
        REQUIRE(!state->is_pending());
    }
}

TEST_CASE("Regression: void future state transitions", "[void][state][regression]") {
    auto* resource = std::pmr::get_default_resource();

    SECTION("set_ready()") {
        auto* state = allocate_future_state<void>(resource);
        intrusive_ptr<future_state_base> holder(state, false);

        REQUIRE(state->is_pending());
        state->set_ready();
        REQUIRE(state->is_ready());
        REQUIRE(state->is_available());
    }

    SECTION("error()") {
        auto* state = allocate_future_state<void>(resource);
        intrusive_ptr<future_state_base> holder(state, false);

        REQUIRE(state->is_pending());
        state->error(std::make_error_code(std::errc::invalid_argument));
        REQUIRE(state->is_error());
        REQUIRE(state->is_available());
    }

    SECTION("cancelled()") {
        auto* state = allocate_future_state<void>(resource);
        intrusive_ptr<future_state_base> holder(state, false);

        REQUIRE(state->is_pending());
        state->cancelled();
        REQUIRE(state->is_cancelled());
        REQUIRE(state->is_available());
    }
}