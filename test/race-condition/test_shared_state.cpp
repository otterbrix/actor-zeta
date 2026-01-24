#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/detail/future.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <memory_resource>
#include <system_error>
#include <coroutine>

// =============================================================================
// Tests for shared_state<T> architecture as described in:
//   docs/actor-zeta-race-comprehensive-fix.md
//
// These tests use the NEW API that will be implemented:
// - state_flags namespace with bit flags (§4.1)
// - shared_state<T> with release_promise/release_future (§5.1)
// - is_ready() checks promise_released, not value_set (§7.3)
//
// IMPORTANT: These tests will NOT COMPILE until the document is implemented.
// This is intentional - they serve as a specification for the new API.
// =============================================================================

using namespace actor_zeta;
using namespace actor_zeta::detail;

// =============================================================================
// TEST SECTION 1: state_flags existence (§4.1)
// =============================================================================

TEST_CASE("state_flags: basic flag values should exist") {
    // §4.1: State flags as bit values
    REQUIRE(state_flags::empty == 0b0000'0000);
    REQUIRE(state_flags::value_set == 0b0000'0001);
    REQUIRE(state_flags::error_set == 0b0000'0010);
    REQUIRE(state_flags::consumed == 0b0000'0100);
    REQUIRE(state_flags::promise_released == 0b0000'1000);
    REQUIRE(state_flags::future_released == 0b0001'0000);
}

TEST_CASE("state_flags: composite masks") {
    REQUIRE(state_flags::result_set == (state_flags::value_set | state_flags::error_set));
    REQUIRE(state_flags::both_released == (state_flags::promise_released | state_flags::future_released));
}

TEST_CASE("state_flags: flags are non-overlapping") {
    REQUIRE((state_flags::value_set & state_flags::error_set) == 0);
    REQUIRE((state_flags::value_set & state_flags::promise_released) == 0);
    REQUIRE((state_flags::value_set & state_flags::future_released) == 0);
    REQUIRE((state_flags::error_set & state_flags::promise_released) == 0);
    REQUIRE((state_flags::promise_released & state_flags::future_released) == 0);
}

// =============================================================================
// TEST SECTION 2: shared_state<T> basic operations (§5.1)
// =============================================================================

TEST_CASE("shared_state<int>: initial state") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    REQUIRE(state->flags_.load() == state_flags::empty);
    REQUIRE_FALSE(state->is_ready());
    REQUIRE_FALSE(state->has_result());
    REQUIRE_FALSE(state->has_error());
    REQUIRE(state->continuation_.load() == nullptr);

    // Cleanup: both release
    state->release_promise();
    state->release_future();
}

TEST_CASE("shared_state<int>: set_value") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    state->set_value(42);

    REQUIRE(state->has_result());
    REQUIRE_FALSE(state->has_error());
    REQUIRE_FALSE(state->is_ready());  // is_ready checks promise_released!
    REQUIRE(state->get_value() == 42);

    state->release_promise();
    REQUIRE(state->is_ready());  // Now ready

    state->release_future();  // Deallocates
}

TEST_CASE("shared_state<int>: take_value") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    state->set_value(123);

    int value = state->take_value();
    REQUIRE(value == 123);

    state->release_promise();
    state->release_future();
}

TEST_CASE("shared_state<int>: set_error") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    auto ec = std::make_error_code(std::errc::operation_canceled);
    state->set_error(ec);

    REQUIRE(state->has_result());  // result_set includes error_set
    REQUIRE(state->has_error());
    REQUIRE(state->get_error() == ec);

    state->release_promise();
    state->release_future();
}

// =============================================================================
// TEST SECTION 3: shared_state<void> operations
// =============================================================================

TEST_CASE("shared_state<void>: initial state") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<void>(resource);

    REQUIRE(state->flags_.load() == state_flags::empty);
    REQUIRE_FALSE(state->is_ready());
    REQUIRE_FALSE(state->has_result());

    state->release_promise();
    state->release_future();
}

TEST_CASE("shared_state<void>: set_value") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<void>(resource);

    state->set_value();

    REQUIRE(state->has_result());
    REQUIRE_FALSE(state->is_ready());  // is_ready checks promise_released

    state->release_promise();
    REQUIRE(state->is_ready());

    state->release_future();
}

// =============================================================================
// TEST SECTION 4: Last-One-Out ownership model (§5.1)
// =============================================================================

TEST_CASE("Last-One-Out: promise releases first") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    state->set_value(42);

    // Promise releases first
    state->release_promise();

    // State should still be accessible
    REQUIRE(state->is_ready());
    REQUIRE(state->has_result());
    REQUIRE(state->get_value() == 42);

    // Future releases — deallocates
    state->release_future();
    // State is now deallocated — no access!
}

TEST_CASE("Last-One-Out: future releases first") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    state->set_value(99);

    // Future releases first
    state->release_future();

    // State should still be accessible (promise hasn't released)
    REQUIRE_FALSE(state->is_ready());  // promise not released yet
    REQUIRE(state->has_result());

    // Promise releases — deallocates
    state->release_promise();
    // State is now deallocated — no access!
}

// =============================================================================
// TEST SECTION 5: is_ready() vs has_result() semantics (§7.3)
// This is the KEY insight of the race condition fix
// =============================================================================

TEST_CASE("is_ready checks promise_released, has_result checks value/error") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    // Initially: nothing set
    REQUIRE_FALSE(state->is_ready());
    REQUIRE_FALSE(state->has_result());

    // After set_value: has_result=true, is_ready=false
    state->set_value(42);
    REQUIRE_FALSE(state->is_ready());
    REQUIRE(state->has_result());

    // After release_promise: is_ready=true
    state->release_promise();
    REQUIRE(state->is_ready());
    REQUIRE(state->has_result());

    state->release_future();
}

// =============================================================================
// TEST SECTION 6: Continuation (CAS-based awaiter support)
// =============================================================================

TEST_CASE("shared_state: continuation atomic operations") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    // Initially null
    REQUIRE(state->continuation_.load() == nullptr);

    // Simulated awaiter: CAS to set continuation
    std::coroutine_handle<> dummy_handle = std::noop_coroutine();

    std::coroutine_handle<> expected = nullptr;
    bool cas_success = state->continuation_.compare_exchange_strong(
        expected, dummy_handle,
        std::memory_order_acq_rel, std::memory_order_acquire);

    REQUIRE(cas_success);
    REQUIRE(state->continuation_.load() == dummy_handle);

    // Producer: exchange to take continuation
    auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
    REQUIRE(cont == dummy_handle);
    REQUIRE(state->continuation_.load() == nullptr);

    state->release_promise();
    state->release_future();
}

TEST_CASE("shared_state: double CAS detects double-await") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    std::coroutine_handle<> handle1 = std::noop_coroutine();
    std::coroutine_handle<> handle2 = std::noop_coroutine();

    // First CAS succeeds
    std::coroutine_handle<> expected1 = nullptr;
    bool cas1 = state->continuation_.compare_exchange_strong(
        expected1, handle1,
        std::memory_order_acq_rel, std::memory_order_acquire);
    REQUIRE(cas1);

    // Second CAS fails (someone already set continuation)
    std::coroutine_handle<> expected2 = nullptr;
    bool cas2 = state->continuation_.compare_exchange_strong(
        expected2, handle2,
        std::memory_order_acq_rel, std::memory_order_acquire);
    REQUIRE_FALSE(cas2);
    REQUIRE(expected2 == handle1);  // expected updated to current value

    state->release_promise();
    state->release_future();
}

// =============================================================================
// TEST SECTION 7: Concurrent operations stress tests
// =============================================================================

TEST_CASE("shared_state: concurrent set_value and release") {
    constexpr int NUM_ITERATIONS = 10000;
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

        std::thread t1([state, i]() {
            state->set_value(int(i));
            state->release_promise();
        });

        std::thread t2([state]() {
            // Spin until result is set
            while (!state->has_result()) {
                std::this_thread::yield();
            }
            state->release_future();
        });

        t1.join();
        t2.join();
    }

    REQUIRE(deallocation_count.load() == NUM_ITERATIONS);
}

TEST_CASE("shared_state: concurrent release_promise and release_future") {
    constexpr int NUM_ITERATIONS = 10000;
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
            state->release_promise();
        });

        std::thread t2([state]() {
            state->release_future();
        });

        t1.join();
        t2.join();
    }

    // Exactly one deallocation per state — Last-One-Out guarantee
    REQUIRE(deallocation_count.load() == NUM_ITERATIONS);
}

// =============================================================================
// TEST SECTION 8: Memory ordering verification
// =============================================================================

TEST_CASE("shared_state: memory ordering - value visible after has_result") {
    constexpr int NUM_ITERATIONS = 10000;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto* resource = std::pmr::get_default_resource();
        auto* state = allocate_shared_state<int>(resource);

        std::atomic<bool> writer_done{false};
        std::atomic<bool> reader_done{false};
        std::atomic<int> read_value{-1};

        std::thread writer([state, i, &writer_done]() {
            state->set_value(int(i));
            writer_done.store(true, std::memory_order_release);
        });

        std::thread reader([state, &reader_done, &read_value]() {
            // Wait for has_result()
            while (!state->has_result()) {
                std::this_thread::yield();
            }
            // Value must be visible now (release-acquire synchronization)
            read_value.store(state->get_value(), std::memory_order_relaxed);
            reader_done.store(true, std::memory_order_release);
        });

        writer.join();
        reader.join();

        REQUIRE(read_value.load() == i);

        state->release_promise();
        state->release_future();
    }
}

// =============================================================================
// TEST SECTION 9: Static assertions (compile-time checks)
// =============================================================================

TEST_CASE("static assertions: lock-free atomics") {
    STATIC_REQUIRE(std::atomic<std::uint8_t>::is_always_lock_free);

    if constexpr (std::atomic<std::coroutine_handle<>>::is_always_lock_free) {
        SUCCEED("coroutine_handle atomic is lock-free");
    } else {
        WARN("coroutine_handle atomic is NOT lock-free on this platform");
    }
}

// =============================================================================
// TEST SECTION 10: Complex types
// =============================================================================

TEST_CASE("shared_state<string>: non-trivial type") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<std::string>(resource);

    std::string test_value = "Hello, World! This is a test string.";
    state->set_value(std::string(test_value));

    REQUIRE(state->has_result());
    REQUIRE(state->get_value() == test_value);

    std::string taken = state->take_value();
    REQUIRE(taken == test_value);

    state->release_promise();
    state->release_future();
}

TEST_CASE("shared_state<vector>: container type") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<std::vector<int>>(resource);

    std::vector<int> test_value = {1, 2, 3, 4, 5};
    state->set_value(std::vector<int>(test_value));

    REQUIRE(state->has_result());
    REQUIRE(state->get_value() == test_value);

    state->release_promise();
    state->release_future();
}