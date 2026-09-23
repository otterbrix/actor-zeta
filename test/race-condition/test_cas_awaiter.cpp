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

// The awaiter protocol, hand-rolled on shared_state so each step can be pinned:
// await_ready() checks has_result(), NOT is_ready(); await_suspend() installs the
// continuation with a CAS (a failed CAS is a double-await) and re-checks
// has_result() afterwards to catch a late producer; final_awaiter claims the
// continuation with an exchange.

using namespace actor_zeta;
using namespace actor_zeta::detail;

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

    std::coroutine_handle<> expected1 = nullptr;
    bool cas1 = state->continuation_.compare_exchange_strong(
        expected1, handle1,
        std::memory_order_acq_rel, std::memory_order_acquire);
    REQUIRE(cas1);

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

    auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);

    REQUIRE(cont == handle);
    REQUIRE(state->continuation_.load() == nullptr);

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    state->release_future();
}

TEST_CASE("CAS awaiter: await_ready returns true if has_result") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    REQUIRE_FALSE(state->has_result());

    state->set_value(42);

    REQUIRE(state->has_result());

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    state->release_future();
}

TEST_CASE("CAS awaiter: await_ready uses has_result, not is_ready") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    state->set_value(42);

    // The state await_ready() sees on an already-completed producer: a result,
    // but no promise_released yet.
    REQUIRE(state->has_result());
    REQUIRE_FALSE(state->is_ready());

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    state->release_future();
}

TEST_CASE("CAS awaiter: await_suspend - producer finished before CAS") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    state->set_value(42);
    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);

    std::coroutine_handle<> handle = std::noop_coroutine();
    std::coroutine_handle<> expected = nullptr;

    bool cas_success = state->continuation_.compare_exchange_strong(
        expected, handle,
        std::memory_order_acq_rel, std::memory_order_acquire);

    REQUIRE(cas_success);

    // The post-CAS re-check: a result is present, so await_suspend() would return
    // false and take its continuation back.
    REQUIRE(state->has_result());

    state->release_future();
}

TEST_CASE("CAS awaiter: await_suspend - producer not finished yet") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    std::coroutine_handle<> handle = std::noop_coroutine();
    std::coroutine_handle<> expected = nullptr;

    bool cas_success = state->continuation_.compare_exchange_strong(
        expected, handle,
        std::memory_order_acq_rel, std::memory_order_acquire);

    REQUIRE(cas_success);

    // The post-CAS re-check: no result, so await_suspend() would stay suspended.
    REQUIRE_FALSE(state->has_result());

    state->set_value(99);

    auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
    REQUIRE(cont == handle);

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    state->release_future();
}

// final_awaiter's order: claim the continuation FIRST, then release_promise().
TEST_CASE("CAS awaiter: final_awaiter takes continuation") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    std::coroutine_handle<> handle = std::noop_coroutine();
    state->continuation_.store(handle, std::memory_order_release);

    auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
    REQUIRE(cont == handle);

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    REQUIRE(state->is_ready());

    REQUIRE(cont != nullptr);

    state->release_future();
}

TEST_CASE("CAS awaiter: final_awaiter with no waiter") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    REQUIRE(state->continuation_.load() == nullptr);

    auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
    REQUIRE(cont == nullptr);

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    REQUIRE(state->is_ready());

    state->release_future();
}

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

            while (!state->is_ready()) {
                std::this_thread::yield();
            }

            // Latched, not asserted here: a Catch2 macro inside a thread is itself
            // a race -- the very thing the comment above already says.
            consumer_resumed.store(state->has_result(), std::memory_order_release);
        });

        producer.join();
        consumer.join();

        REQUIRE(consumer_resumed.load());
        REQUIRE_FALSE(promise_deallocated.load());

        state->release_future();
    }
}

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
            state->set_value(int(i));

            state->continuation_.exchange(nullptr, std::memory_order_acq_rel);

            promise_deallocated.store(state->release_promise(), std::memory_order_relaxed);
        });

        std::thread consumer([state, &read_value]() {
            std::coroutine_handle<> handle = std::noop_coroutine();
            state->continuation_.store(handle, std::memory_order_release);

            while (!state->is_ready()) {
                std::this_thread::yield();
            }

            // The value written before the exchange must be visible after is_ready().
            read_value.store(state->get_value(), std::memory_order_relaxed);
        });

        producer.join();
        consumer.join();

        REQUIRE(read_value.load() == i);
        REQUIRE_FALSE(promise_deallocated.load());

        state->release_future();
    }
}

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