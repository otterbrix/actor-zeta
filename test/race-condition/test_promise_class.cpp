#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/detail/future.hpp>
#include <actor-zeta/detail/shared_state.hpp>

#include <atomic>
#include <memory_resource>
#include <system_error>
#include <thread>
#include <vector>
#include <coroutine>

// =============================================================================
// Tests for promise<T> class as described in:
//   docs/actor-zeta-race-comprehensive-fix.md §12.3
//
// These tests use the PUBLIC API of promise<T>
// =============================================================================

using namespace actor_zeta;
using namespace actor_zeta::detail;

// =============================================================================
// TEST SECTION 1: Basic promise<T> creation (§12.3)
// =============================================================================

TEST_CASE("promise<int>: construction allocates shared_state") {
    auto* resource = std::pmr::get_default_resource();

    // New API: promise<T>(resource) allocates shared_state
    promise<int> p(resource);

    REQUIRE(p.valid());
    REQUIRE(p.internal_state() != nullptr);
}

TEST_CASE("promise<void>: construction allocates shared_state") {
    auto* resource = std::pmr::get_default_resource();

    promise<void> p(resource);

    REQUIRE(p.valid());
    REQUIRE(p.internal_state() != nullptr);
}

TEST_CASE("promise<int>: construction from existing state") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    // New API: promise<T>(state) takes existing state
    promise<int> p(state);

    REQUIRE(p.valid());
    REQUIRE(p.internal_state() == state);
}

// =============================================================================
// TEST SECTION 2: get_future (§12.3)
// =============================================================================

TEST_CASE("promise<int>: get_future creates future sharing state") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p(resource);
    auto* original_state = p.internal_state();

    unique_future<int> future = p.get_future();

    REQUIRE(future.valid());
    REQUIRE(future.internal_state() == original_state);
}

// =============================================================================
// TEST SECTION 3: set_value behavior (§12.3)
// set_value() writes value, calls release_promise(), nulls state_
// =============================================================================

TEST_CASE("promise<int>: set_value stores value and releases") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p(resource);
    auto* state = p.internal_state();
    auto future = p.get_future();

    p.set_value(42);

    // Value should be stored
    REQUIRE(state->has_result());
    REQUIRE(state->get_value() == 42);

    // Promise released (is_ready becomes true)
    REQUIRE(state->is_ready());

    // promise should be invalid after set_value
    REQUIRE_FALSE(p.valid());
}

TEST_CASE("promise<void>: set_value releases") {
    auto* resource = std::pmr::get_default_resource();

    promise<void> p(resource);
    auto* state = p.internal_state();
    auto future = p.get_future();

    p.set_value();

    REQUIRE(state->has_result());
    REQUIRE(state->is_ready());
    REQUIRE_FALSE(p.valid());
}

TEST_CASE("promise<string>: set_value with complex type") {
    auto* resource = std::pmr::get_default_resource();

    promise<std::string> p(resource);
    auto future = p.get_future();

    std::string value = "Hello, World!";
    p.set_value(std::move(value));

    REQUIRE_FALSE(p.valid());

    // Value already set above: future is ready immediately. Poll then take.
    while (!future.is_ready()) {
        std::this_thread::yield();
    }
    std::string result = std::move(future).take_ready();
    REQUIRE(result == "Hello, World!");
}

// =============================================================================
// TEST SECTION 4: error behavior (§12.3)
// =============================================================================

TEST_CASE("promise<int>: set_error sets error and releases") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p(resource);
    auto* state = p.internal_state();
    auto future = p.get_future();

    auto ec = std::make_error_code(std::errc::operation_canceled);
    p.error(ec);

    REQUIRE(state->has_error());
    REQUIRE(state->get_error() == ec);
    REQUIRE(state->is_ready());
    REQUIRE_FALSE(p.valid());
}

// =============================================================================
// TEST SECTION 5: Move semantics (§12.3)
// =============================================================================

TEST_CASE("promise<int>: move construction") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p1(resource);
    auto* original_state = p1.internal_state();
    auto future = p1.get_future();

    promise<int> p2(std::move(p1));

    REQUIRE_FALSE(p1.valid());  // Moved-from

    REQUIRE(p2.internal_state() == original_state);
    REQUIRE(p2.valid());

    p2.set_value(123);
    REQUIRE(future.is_ready());
    REQUIRE(std::move(future).take_ready() == 123);
}

TEST_CASE("promise<int>: move assignment") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p1(resource);
    auto future1 = p1.get_future();

    promise<int> p2(resource);
    auto future2 = p2.get_future();

    // Move p1 into p2 — p2's old state should get error
    p2 = std::move(p1);

    REQUIRE_FALSE(p1.valid());
    REQUIRE(p2.valid());

    p2.set_value(456);
    REQUIRE(future1.is_ready());
    REQUIRE(std::move(future1).take_ready() == 456);

    // p2's old state should have received error (broken_pipe) in destructor
    REQUIRE(future2.is_ready());
    REQUIRE(future2.failed());
    REQUIRE(future2.error() == std::make_error_code(std::errc::broken_pipe));
}

// =============================================================================
// TEST SECTION 6: Destructor behavior (§12.3)
// If promise destroyed without set_value → sets error broken_pipe
// =============================================================================

TEST_CASE("promise<int>: destructor without set_value sets broken_pipe") {
    auto* resource = std::pmr::get_default_resource();

    unique_future<int> future([resource]() {
        promise<int> p(resource);
        return p.get_future();
        // p destroyed here without set_value
    }());

    // Future should indicate failure with broken_pipe
    REQUIRE(future.is_ready());
    REQUIRE(future.failed());
    REQUIRE(future.error() == std::make_error_code(std::errc::broken_pipe));
}

TEST_CASE("promise<int>: destructor after set_value does nothing") {
    auto* resource = std::pmr::get_default_resource();

    unique_future<int> future([resource]() {
        promise<int> p(resource);
        auto f = p.get_future();
        p.set_value(42);  // state_ becomes nullptr
        return f;
        // p destroyed here, but state_ is null so nothing happens
    }());

    REQUIRE(future.is_ready());
    REQUIRE_FALSE(future.failed());
    REQUIRE(std::move(future).take_ready() == 42);
}

// =============================================================================
// TEST SECTION 7: Concurrent promise operations
// =============================================================================

TEST_CASE("promise<int>: concurrent set_value and future read") {
    constexpr int NUM_ITERATIONS = 1000;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto* resource = std::pmr::get_default_resource();
        promise<int> p(resource);
        auto future = p.get_future();

        std::atomic<int> result{-1};

        std::thread writer([&p, i]() {
            p.set_value(i);
        });

        // Reader kept on its own thread (cross-thread producer/consumer): the
        // writer thread sets the value, the reader just polls readiness and takes.
        std::thread reader([&future, &result]() {
            while (!future.is_ready()) {
                std::this_thread::yield();
            }
            int r = std::move(future).take_ready();
            result.store(r, std::memory_order_release);
        });

        writer.join();
        reader.join();

        REQUIRE(result.load() == i);
    }
}

// =============================================================================
// TEST SECTION 8: unique_future basic operations
// =============================================================================

TEST_CASE("unique_future<int>: initial state from promise") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p(resource);
    auto future = p.get_future();

    REQUIRE(future.valid());
    REQUIRE(future.internal_state() != nullptr);
    REQUIRE_FALSE(future.is_ready());
    REQUIRE_FALSE(future.failed());
}

TEST_CASE("unique_future<int>: available checks is_ready") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p(resource);
    auto future = p.get_future();

    // Before set_value: not available (is_ready = false)
    REQUIRE_FALSE(future.is_ready());

    p.set_value(42);

    // After set_value (which calls release_promise): available
    REQUIRE(future.is_ready());
}

TEST_CASE("unique_future<int>: move semantics") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p(resource);
    auto future1 = p.get_future();
    auto* original_state = future1.internal_state();

    auto future2 = std::move(future1);

    REQUIRE_FALSE(future1.valid());

    REQUIRE(future2.internal_state() == original_state);
    REQUIRE(future2.valid());

    p.set_value(789);
    REQUIRE(future2.is_ready());
}

TEST_CASE("unique_future: destructor calls release_future") {
    auto* resource = std::pmr::get_default_resource();
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

    tracking_resource tracked(resource, &deallocation_count);

    {
        promise<int> p(&tracked);
        auto future = p.get_future();
        p.set_value(42);
        // future destroyed here — calls release_future
        // promise already released in set_value
        // Last-One-Out deallocates
    }

    REQUIRE(deallocation_count.load() == 1);
}

// =============================================================================
// TEST SECTION 9: make_ready_future helper
// =============================================================================

TEST_CASE("make_ready_future<int>: creates ready future") {
    auto* resource = std::pmr::get_default_resource();

    auto future = make_ready_future<int>(resource, 42);

    REQUIRE(future.valid());
    REQUIRE(future.is_ready());  // is_ready = true
    REQUIRE(std::move(future).take_ready() == 42);
}

TEST_CASE("make_ready_future<void>: creates ready void future") {
    auto* resource = std::pmr::get_default_resource();

    auto future = make_ready_future(resource);

    REQUIRE(future.valid());
    REQUIRE(future.is_ready());
}

// =============================================================================
// TEST SECTION 10: make_error helper
// =============================================================================

TEST_CASE("make_error<int>: creates failed future") {
    auto* resource = std::pmr::get_default_resource();

    auto ec = std::make_error_code(std::errc::invalid_argument);
    auto future = make_error<int>(resource, ec);

    REQUIRE(future.valid());
    REQUIRE(future.is_ready());  // is_ready = true
    REQUIRE(future.failed());
    REQUIRE(future.error() == ec);
}

// =============================================================================
// TEST SECTION 11: Ownership transfer pattern (§12.3)
// =============================================================================

TEST_CASE("promise ownership: set_value invalidates promise") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p(resource);
    REQUIRE(p.valid());

    p.set_value(42);

    // After set_value, promise should be invalid
    REQUIRE_FALSE(p.valid());
}

// =============================================================================
// TEST SECTION 12: dispatch() pattern simulation
// =============================================================================

TEST_CASE("promise: dispatch pattern") {
    auto* resource = std::pmr::get_default_resource();

    // Caller creates promise+future pair
    auto* state = allocate_shared_state<int>(resource);

    promise<int> caller_promise(state);
    unique_future<int> caller_future(state);

    // dispatch gets the value from method and sets it
    int value_from_method = 42;
    caller_promise.set_value(std::move(value_from_method));

    // Caller reads from their future
    REQUIRE(caller_future.is_ready());
    REQUIRE(std::move(caller_future).take_ready() == 42);
}

// =============================================================================
// TEST SECTION 13: queue_closed pattern
// =============================================================================

TEST_CASE("promise: queue_closed - future gets error") {
    auto* resource = std::pmr::get_default_resource();

    unique_future<int> future([resource]() {
        promise<int> p(resource);
        auto f = p.get_future();
        // Promise destroyed without set_value (simulating queue_closed)
        return f;
    }());

    // Future should indicate failure with broken_pipe
    REQUIRE(future.is_ready());
    REQUIRE(future.failed());
    REQUIRE(future.error() == std::make_error_code(std::errc::broken_pipe));
}
