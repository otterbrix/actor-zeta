#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/detail/future.hpp>

#include <atomic>
#include <chrono>
#include <memory_resource>
#include <system_error>
#include <thread>
#include <vector>
#include <coroutine>
#include <functional>

// =============================================================================
// The CAS-based awaiter protocol these tests pin:
// - await_ready() checks has_result(), NOT is_ready()
// - await_suspend() installs the continuation with a CAS; a failed CAS means a
//   second awaiter, i.e. a double-await (programmer error)
// - after a successful CAS, re-check has_result() to catch a late producer
// - final_awaiter takes the continuation with an exchange, atomically
// =============================================================================

using namespace actor_zeta;
using namespace actor_zeta::detail;

// =============================================================================
// TEST SECTION 1: shared_state::continuation_ CAS operations
// =============================================================================

TEST_CASE("CAS awaiter: initial continuation is nullptr") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    REQUIRE(state->continuation_.load() == nullptr);

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    state->release_future();
}

TEST_CASE("CAS awaiter: CAS sets continuation successfully") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    std::coroutine_handle<> handle = std::noop_coroutine();

    // Simulate awaiter: CAS to set continuation
    std::coroutine_handle<> expected = nullptr;
    bool cas_success = state->continuation_.compare_exchange_strong(
        expected, handle,
        std::memory_order_acq_rel, std::memory_order_acquire);

    REQUIRE(cas_success);
    REQUIRE(state->continuation_.load() == handle);

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    state->release_future();
}

TEST_CASE("CAS awaiter: second CAS fails (double-await detection)") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    std::coroutine_handle<> handle1 = std::noop_coroutine();
    std::coroutine_handle<> handle2 = std::noop_coroutine();

    // First awaiter succeeds
    std::coroutine_handle<> expected1 = nullptr;
    bool cas1 = state->continuation_.compare_exchange_strong(
        expected1, handle1,
        std::memory_order_acq_rel, std::memory_order_acquire);
    REQUIRE(cas1);

    // Second awaiter fails — double-await detected
    std::coroutine_handle<> expected2 = nullptr;
    bool cas2 = state->continuation_.compare_exchange_strong(
        expected2, handle2,
        std::memory_order_acq_rel, std::memory_order_acquire);
    REQUIRE_FALSE(cas2);
    REQUIRE(expected2 == handle1);  // expected updated to current value

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    state->release_future();
}

TEST_CASE("CAS awaiter: exchange takes continuation atomically") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    std::coroutine_handle<> handle = std::noop_coroutine();
    state->continuation_.store(handle, std::memory_order_release);

    // Producer: exchange to take continuation
    auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);

    REQUIRE(cont == handle);
    REQUIRE(state->continuation_.load() == nullptr);

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    state->release_future();
}

// =============================================================================
// TEST SECTION 2: await_ready behavior — has_result() is the fast path
// =============================================================================

TEST_CASE("CAS awaiter: await_ready returns true if has_result") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    // Initially: no result
    REQUIRE_FALSE(state->has_result());

    // Set value
    state->set_value(42);

    // Now has_result is true — await_ready should return true
    REQUIRE(state->has_result());

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    state->release_future();
}

TEST_CASE("CAS awaiter: await_ready uses has_result, not is_ready") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    state->set_value(42);

    // has_result is true, but is_ready is false (promise not released)
    REQUIRE(state->has_result());
    REQUIRE_FALSE(state->is_ready());

    // await_ready should check has_result → returns true
    // This is the fast path for already-completed coroutines

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    state->release_future();
}

// =============================================================================
// TEST SECTION 3: await_suspend flow
// 1. CAS to set continuation
// 2. Re-check has_result after CAS
// 3. Return true (suspend) or false (resume immediately)
// =============================================================================

TEST_CASE("CAS awaiter: await_suspend - producer finished before CAS") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    // Producer finishes first
    state->set_value(42);
    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);

    // Awaiter comes late
    std::coroutine_handle<> handle = std::noop_coroutine();
    std::coroutine_handle<> expected = nullptr;

    bool cas_success = state->continuation_.compare_exchange_strong(
        expected, handle,
        std::memory_order_acq_rel, std::memory_order_acquire);

    REQUIRE(cas_success);

    // Re-check has_result after CAS
    REQUIRE(state->has_result());

    // Since has_result is true, awaiter should return false (resume immediately)
    // and take back its continuation

    state->release_future();
}

TEST_CASE("CAS awaiter: await_suspend - producer not finished yet") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    // Awaiter comes first
    std::coroutine_handle<> handle = std::noop_coroutine();
    std::coroutine_handle<> expected = nullptr;

    bool cas_success = state->continuation_.compare_exchange_strong(
        expected, handle,
        std::memory_order_acq_rel, std::memory_order_acquire);

    REQUIRE(cas_success);

    // Re-check has_result after CAS
    REQUIRE_FALSE(state->has_result());

    // Since has_result is false, awaiter returns true (suspends)
    // Producer will resume via symmetric transfer

    // Producer finishes
    state->set_value(99);

    // Producer takes continuation
    auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
    REQUIRE(cont == handle);

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    state->release_future();
}

// =============================================================================
// TEST SECTION 4: final_awaiter flow
// 1. Take continuation via exchange
// 2. Release promise (Last-One-Out)
// 3. Self-destroy coroutine
// 4. Symmetric transfer to continuation
// =============================================================================

TEST_CASE("CAS awaiter: final_awaiter takes continuation") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    std::coroutine_handle<> handle = std::noop_coroutine();
    state->continuation_.store(handle, std::memory_order_release);

    // final_suspend: take continuation first
    auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
    REQUIRE(cont == handle);

    // Then release promise
    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    REQUIRE(state->is_ready());

    // Then self.destroy() (simulated - we can't test actual destroy)

    // Then symmetric transfer to cont
    REQUIRE(cont != nullptr);

    state->release_future();
}

TEST_CASE("CAS awaiter: final_awaiter with no waiter") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    // No awaiter registered
    REQUIRE(state->continuation_.load() == nullptr);

    // final_suspend: take continuation
    auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
    REQUIRE(cont == nullptr);

    // Then release promise
    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    REQUIRE(state->is_ready());

    // Symmetric transfer to noop_coroutine

    state->release_future();
}

// =============================================================================
// TEST SECTION 5: Concurrent CAS stress tests
// =============================================================================

TEST_CASE("CAS awaiter: concurrent CAS - only one succeeds") {
    constexpr int NUM_ITERATIONS = 1000;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto* resource = std::pmr::get_default_resource();
        auto* state = allocate_shared_state<int>(resource);

        std::atomic<int> success_count{0};
        std::atomic<int> failure_count{0};

        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([state, &success_count, &failure_count]() {
                std::coroutine_handle<> handle = std::noop_coroutine();
                std::coroutine_handle<> expected = nullptr;

                bool cas_success = state->continuation_.compare_exchange_strong(
                    expected, handle,
                    std::memory_order_acq_rel, std::memory_order_acquire);

                if (cas_success) {
                    success_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    failure_count.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        // Exactly one thread should succeed
        REQUIRE(success_count.load() == 1);
        REQUIRE(failure_count.load() == 3);

        const bool deallocated = state->release_promise();
        REQUIRE_FALSE(deallocated);
        state->release_future();
    }
}

TEST_CASE("CAS awaiter: concurrent producer-consumer") {
    constexpr int NUM_ITERATIONS = 1000;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto* resource = std::pmr::get_default_resource();
        auto* state = allocate_shared_state<int>(resource);

        std::atomic<bool> consumer_resumed{false};
        // release_promise() reports whether THIS call deallocated the state. The
        // peer thread here only reads -- it never releases the future -- so the
        // answer is deterministically false. Latch it and assert after the join:
        // a Catch2 macro fired from inside a thread is itself a data race.
        std::atomic<bool> promise_deallocated{false};

        std::thread producer([state, i, &promise_deallocated]() {
            state->set_value(int(i));

            // Take the continuation. This test exercises the exchange only and
            // deliberately does not resume it.
            state->continuation_.exchange(nullptr, std::memory_order_acq_rel);

            promise_deallocated.store(state->release_promise(), std::memory_order_relaxed);
        });

        std::thread consumer([state, &consumer_resumed]() {
            std::coroutine_handle<> handle = std::noop_coroutine();
            std::coroutine_handle<> expected = nullptr;

            state->continuation_.compare_exchange_strong(
                expected, handle,
                std::memory_order_acq_rel, std::memory_order_acquire);

            // Wait for is_ready
            while (!state->is_ready()) {
                std::this_thread::yield();
            }

            // Value should be visible
            REQUIRE(state->has_result());
            consumer_resumed.store(true, std::memory_order_release);
        });

        producer.join();
        consumer.join();

        REQUIRE(consumer_resumed.load());
        REQUIRE_FALSE(promise_deallocated.load());

        state->release_future();
    }
}

// =============================================================================
// TEST SECTION 6: Memory ordering verification
// =============================================================================

TEST_CASE("CAS awaiter: memory ordering - value visible after exchange") {
    constexpr int NUM_ITERATIONS = 1000;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto* resource = std::pmr::get_default_resource();
        auto* state = allocate_shared_state<int>(resource);

        std::atomic<int> read_value{-1};
        // Same shape as above: latch release_promise()'s answer, assert after the
        // join -- a Catch2 macro fired from inside a thread is itself a data race.
        std::atomic<bool> promise_deallocated{false};

        std::thread producer([state, i, &promise_deallocated]() {
            // Write value BEFORE exchange
            state->set_value(int(i));

            // Exchange continuation (with acq_rel)
            state->continuation_.exchange(nullptr, std::memory_order_acq_rel);

            promise_deallocated.store(state->release_promise(), std::memory_order_relaxed);
        });

        std::thread consumer([state, &read_value]() {
            // Set continuation
            std::coroutine_handle<> handle = std::noop_coroutine();
            state->continuation_.store(handle, std::memory_order_release);

            // Wait for is_ready
            while (!state->is_ready()) {
                std::this_thread::yield();
            }

            // Value must be visible (synchronizes-with exchange)
            read_value.store(state->get_value(), std::memory_order_relaxed);
        });

        producer.join();
        consumer.join();

        REQUIRE(read_value.load() == i);
        REQUIRE_FALSE(promise_deallocated.load());

        state->release_future();
    }
}

// =============================================================================
// TEST SECTION 7: Edge cases
// =============================================================================

TEST_CASE("CAS awaiter: multiple set_value forbidden") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    state->set_value(42);
    REQUIRE(state->has_result());

    // A second set_value is a contract violation; nothing in shared_state
    // enforces it, so only the first write is exercised here.

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    state->release_future();
}

TEST_CASE("CAS awaiter: error then value forbidden") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    auto ec = std::make_error_code(std::errc::operation_canceled);
    state->set_error(ec);
    REQUIRE(state->has_error());

    // set_value after set_error is likewise a contract violation and is not
    // exercised; error_set alone is what a consumer must see.

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    state->release_future();
}