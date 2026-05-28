#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/detail/future.hpp>
#include <actor-zeta/detail/shared_state.hpp>

#include <atomic>
#include <thread>
#include <vector>
#include <memory_resource>

using namespace actor_zeta;
using namespace actor_zeta::detail;

// =============================================================================
// Tests for the new shared_state architecture (replacing future_state_fixes)
// =============================================================================

// =============================================================================
// Issue #1: Race between available() and final_suspend
// NEW SOLUTION: is_ready() checks promise_released flag, not has_result()
// =============================================================================

TEST_CASE("Issue #1: is_ready vs has_result distinction", "[race][availability]") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    // Initially both are false
    REQUIRE_FALSE(state->has_result());
    REQUIRE_FALSE(state->is_ready());

    // After set_value, has_result is true but is_ready is still false
    state->set_value(42);
    REQUIRE(state->has_result());
    REQUIRE_FALSE(state->is_ready());  // Promise not released yet!

    // After release_promise, is_ready becomes true
    (void)state->release_promise();
    REQUIRE(state->is_ready());

    // Cleanup
    state->release_future();
}

TEST_CASE("Issue #1: void specialization", "[race][void]") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<void>(resource);

    // Same pattern for void
    REQUIRE_FALSE(state->has_result());
    REQUIRE_FALSE(state->is_ready());

    state->set_value();
    REQUIRE(state->has_result());
    REQUIRE_FALSE(state->is_ready());

    (void)state->release_promise();
    REQUIRE(state->is_ready());

    state->release_future();
}

// =============================================================================
// Issue #2: Data race on error_code write
// NEW SOLUTION: Atomic flags ensure proper ordering
// =============================================================================

TEST_CASE("Issue #2: error state is atomic", "[error][atomic]") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    auto ec = std::make_error_code(std::errc::invalid_argument);
    state->set_error(ec);

    REQUIRE(state->has_error());
    REQUIRE(state->has_result());  // error counts as result
    REQUIRE(state->get_error() == ec);

    (void)state->release_promise();
    state->release_future();
}

// =============================================================================
// Issue #3: Consistent error handling for void/non-void
// =============================================================================

TEST_CASE("Issue #3: error state handling is consistent for int type", "[error][consistency]") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p(resource);
    auto f = p.get_future();  // Get future BEFORE error
    p.error(std::make_error_code(std::errc::invalid_argument));

    // Future should report failed state
    REQUIRE(f.failed());
    REQUIRE(f.error() == std::make_error_code(std::errc::invalid_argument));
}

TEST_CASE("Issue #3: error state handling is consistent for void type", "[error][consistency]") {
    auto* resource = std::pmr::get_default_resource();

    promise<void> p(resource);
    auto f = p.get_future();  // Get future BEFORE error
    p.error(std::make_error_code(std::errc::invalid_argument));

    // Future should report failed state
    REQUIRE(f.failed());
    REQUIRE(f.error() == std::make_error_code(std::errc::invalid_argument));
}

TEST_CASE("Issue #3: cancelled state handling is consistent for int type", "[cancel][consistency]") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p(resource);
    unique_future<int> f = p.get_future();
    // Cancellation is now produced via the promise's error channel.
    p.error(std::make_error_code(std::errc::operation_canceled));

    // Future should report cancelled state (observed via failed()/error())
    REQUIRE(f.failed());
    REQUIRE(f.error() == std::make_error_code(std::errc::operation_canceled));
}

TEST_CASE("Issue #3: cancelled state handling is consistent for void type", "[cancel][consistency]") {
    auto* resource = std::pmr::get_default_resource();

    promise<void> p(resource);
    unique_future<void> f = p.get_future();
    // Cancellation is now produced via the promise's error channel.
    p.error(std::make_error_code(std::errc::operation_canceled));

    // Future should report cancelled state (observed via failed()/error())
    REQUIRE(f.failed());
    REQUIRE(f.error() == std::make_error_code(std::errc::operation_canceled));
}

// =============================================================================
// Issue #4: Last-One-Out deallocation
// =============================================================================

TEST_CASE("Issue #4: Last-One-Out deallocates correctly", "[memory][last-one-out]") {
    std::atomic<int> deallocation_count{0};

    struct tracking_resource : std::pmr::memory_resource {
        std::pmr::memory_resource* upstream_;
        std::atomic<int>* counter_;

        tracking_resource(std::pmr::memory_resource* up, std::atomic<int>* c)
            : upstream_(up), counter_(c) {}

        void* do_allocate(std::size_t bytes, std::size_t align) override {
            return upstream_->allocate(bytes, align);
        }

        void do_deallocate(void* p, std::size_t bytes, std::size_t align) override {
            counter_->fetch_add(1, std::memory_order_relaxed);
            upstream_->deallocate(p, bytes, align);
        }

        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
            return this == &other;
        }
    };

    tracking_resource resource(std::pmr::get_default_resource(), &deallocation_count);

    SECTION("Promise releases first") {
        auto* state = allocate_shared_state<int>(&resource);
        state->set_value(42);
        (void)state->release_promise();
        REQUIRE(deallocation_count.load() == 0);  // Future still holds ref

        state->release_future();
        REQUIRE(deallocation_count.load() == 1);  // Last one out deallocates
    }

    deallocation_count.store(0);

    SECTION("Future releases first") {
        auto* state = allocate_shared_state<int>(&resource);
        state->release_future();
        REQUIRE(deallocation_count.load() == 0);  // Promise still holds ref

        state->set_value(42);
        (void)state->release_promise();
        REQUIRE(deallocation_count.load() == 1);  // Last one out deallocates
    }
}

// =============================================================================
// Issue #5: operator= should not cancel already-completed futures
// =============================================================================

TEST_CASE("Issue #5: operator= does not overwrite error state", "[operator=][error]") {
    auto* resource = std::pmr::get_default_resource();

    // Create first future with error
    promise<int> p1(resource);
    unique_future<int> f1 = p1.get_future();
    p1.error(std::make_error_code(std::errc::invalid_argument));

    // Create second future
    promise<int> p2(resource);
    unique_future<int> f2 = p2.get_future();
    p2.set_value(42);

    // Move f2 into f1 - old state released, new state acquired
    f1 = std::move(f2);

    // f1 now holds f2's state (which has value 42)
    REQUIRE(f1.is_ready());
    REQUIRE(std::move(f1).take_ready() == 42);
}

TEST_CASE("Issue #5: operator= releases old state properly", "[operator=][memory]") {
    std::atomic<int> deallocation_count{0};

    struct tracking_resource : std::pmr::memory_resource {
        std::pmr::memory_resource* upstream_;
        std::atomic<int>* counter_;

        tracking_resource(std::pmr::memory_resource* up, std::atomic<int>* c)
            : upstream_(up), counter_(c) {}

        void* do_allocate(std::size_t bytes, std::size_t align) override {
            return upstream_->allocate(bytes, align);
        }

        void do_deallocate(void* p, std::size_t bytes, std::size_t align) override {
            counter_->fetch_add(1, std::memory_order_relaxed);
            upstream_->deallocate(p, bytes, align);
        }

        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
            return this == &other;
        }
    };

    tracking_resource resource(std::pmr::get_default_resource(), &deallocation_count);

    {
        promise<int> p1(&resource);
        promise<int> p2(&resource);

        unique_future<int> f1 = p1.get_future();
        unique_future<int> f2 = p2.get_future();

        p1.set_value(1);
        p2.set_value(2);

        // Move f2 into f1
        f1 = std::move(f2);

        // f1's old state should be deallocated (promise+future both released)
        REQUIRE(deallocation_count.load() == 1);
    }

    // All states should be deallocated after scope
    REQUIRE(deallocation_count.load() == 2);
}

// =============================================================================
// Concurrent stress tests
// =============================================================================

TEST_CASE("Concurrent: promise and future release race", "[concurrent][memory]") {
    constexpr int NUM_ITERATIONS = 1000;
    std::atomic<int> deallocation_count{0};

    struct tracking_resource : std::pmr::memory_resource {
        std::pmr::memory_resource* upstream_;
        std::atomic<int>* counter_;

        tracking_resource(std::pmr::memory_resource* up, std::atomic<int>* c)
            : upstream_(up), counter_(c) {}

        void* do_allocate(std::size_t bytes, std::size_t align) override {
            return upstream_->allocate(bytes, align);
        }

        void do_deallocate(void* p, std::size_t bytes, std::size_t align) override {
            counter_->fetch_add(1, std::memory_order_relaxed);
            upstream_->deallocate(p, bytes, align);
        }

        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
            return this == &other;
        }
    };

    tracking_resource resource(std::pmr::get_default_resource(), &deallocation_count);

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto* state = allocate_shared_state<int>(&resource);
        state->set_value(int(i));

        std::thread t1([state]() {
            (void)state->release_promise();
        });

        std::thread t2([state]() {
            state->release_future();
        });

        t1.join();
        t2.join();
    }

    // Exactly one deallocation per iteration
    REQUIRE(deallocation_count.load() == NUM_ITERATIONS);
}

TEST_CASE("Concurrent: is_ready polling is safe", "[concurrent][polling]") {
    constexpr int NUM_ITERATIONS = 1000;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto* resource = std::pmr::get_default_resource();
        auto* state = allocate_shared_state<int>(resource);

        std::atomic<bool> producer_done{false};
        std::atomic<int> read_value{-1};

        std::thread producer([state, i, &producer_done]() {
            state->set_value(int(i));
            (void)state->release_promise();
            producer_done.store(true, std::memory_order_release);
        });

        std::thread consumer([state, &read_value]() {
            // Poll is_ready (safe after release_promise)
            while (!state->is_ready()) {
                std::this_thread::yield();
            }
            read_value.store(state->get_value(), std::memory_order_relaxed);
        });

        producer.join();
        consumer.join();

        REQUIRE(read_value.load() == i);
        state->release_future();
    }
}

// Ported from the (deleted) test/slot-refcount release-acquire test against the
// legacy future_state<T>: verify that once is_ready() returns true on shared_state,
// a relaxed-stored side effect from the producer is visible on the reader. This
// exercises the acquire of is_ready() against the release of set_value/release_promise
// (shared_state.hpp:64-67 release; :126 fetch_or acq_rel; :83 acquire load).
TEST_CASE("Concurrent: is_ready acquire synchronizes side effect on shared_state",
          "[concurrent][polling][memory-ordering]") {
    constexpr int NUM_ITERATIONS = 1000;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto* resource = std::pmr::get_default_resource();
        auto* state = allocate_shared_state<int>(resource);

        std::atomic<int> side_effect{0};
        std::atomic<int> read_side_effect{-1};

        std::thread writer([state, &side_effect]() {
            // Relaxed store BEFORE the releasing operations on the state.
            side_effect.store(42, std::memory_order_relaxed);
            state->set_value(100);              // release on flags_
            (void) state->release_promise();    // acq_rel on flags_
        });

        std::thread reader([state, &side_effect, &read_side_effect]() {
            // Acquire is exercised by is_ready() (loads flags_ acquire).
            while (!state->is_ready()) {
                std::this_thread::yield();
            }
            // If is_ready() returned true, the producer's relaxed store must be
            // visible — that is the release/acquire chain under test.
            read_side_effect.store(side_effect.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
        });

        writer.join();
        reader.join();

        REQUIRE(read_side_effect.load(std::memory_order_relaxed) == 42);
        state->release_future();
    }
}

// =============================================================================
// Promise/Future integration tests
// =============================================================================

TEST_CASE("Integration: basic promise-future flow", "[integration]") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p(resource);
    auto f = p.get_future();

    REQUIRE(f.valid());
    REQUIRE_FALSE(f.is_ready());

    p.set_value(42);

    REQUIRE(f.is_ready());
    REQUIRE(std::move(f).take_ready() == 42);
}

TEST_CASE("Integration: promise destruction without set_value", "[integration][error]") {
    auto* resource = std::pmr::get_default_resource();

    unique_future<int> future([resource]() {
        promise<int> p(resource);
        auto f = p.get_future();
        // Promise destroyed without set_value
        return f;
    }());

    // Future should be in failed state with broken_pipe
    REQUIRE(future.is_ready());
    REQUIRE(future.failed());
    REQUIRE(future.error() == std::make_error_code(std::errc::broken_pipe));
}

TEST_CASE("Integration: move semantics", "[integration][move]") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p1(resource);
    auto f1 = p1.get_future();
    auto* original_state = f1.internal_state();

    // Move promise
    promise<int> p2(std::move(p1));
    REQUIRE_FALSE(p1.valid());
    REQUIRE(p2.valid());

    // Move future
    unique_future<int> f2(std::move(f1));
    REQUIRE_FALSE(f1.valid());
    REQUIRE(f2.valid());
    REQUIRE(f2.internal_state() == original_state);

    p2.set_value(123);
    REQUIRE(f2.is_ready());
    REQUIRE(std::move(f2).take_ready() == 123);
}