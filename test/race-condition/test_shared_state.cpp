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

namespace {
    // Which side of the Last-One-Out race the current thread is playing. The
    // tracking resources below stamp it into the iteration's role slot on every
    // deallocation, which is what makes release_promise()'s self-report
    // CHECKABLE: "I deallocated" must coincide with "this thread deallocated".
    // Without the attribution, any assertion on the counter is vacuous -- a
    // release_promise() hard-coded to `return true` would satisfy it.
    thread_local int tls_release_role = 0;   // 1 = promise side, 2 = future side
    constexpr int role_promise = 1;
    constexpr int role_future = 2;
} // namespace


using namespace actor_zeta;
using namespace actor_zeta::detail;

// =============================================================================
// TEST SECTION 1: state_flags existence
// =============================================================================

TEST_CASE("state_flags: basic flag values should exist") {
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
// TEST SECTION 2: shared_state<T> basic operations
// =============================================================================

TEST_CASE("shared_state<int>: initial state") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    REQUIRE(state->flags_.load() == state_flags::empty);
    REQUIRE_FALSE(state->is_ready());
    REQUIRE_FALSE(state->has_result());
    REQUIRE_FALSE(state->has_error());
    REQUIRE(state->continuation_.load() == nullptr);

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
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

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    REQUIRE(state->is_ready());

    state->release_future();  // last one out: deallocates
}

TEST_CASE("shared_state<int>: take_value") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    state->set_value(123);

    int value = state->take_value();
    REQUIRE(value == 123);

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
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

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
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

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    state->release_future();
}

TEST_CASE("shared_state<void>: set_value") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<void>(resource);

    state->set_value();

    REQUIRE(state->has_result());
    REQUIRE_FALSE(state->is_ready());  // is_ready checks promise_released

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    REQUIRE(state->is_ready());

    state->release_future();
}

// =============================================================================
// TEST SECTION 4: Last-One-Out ownership model
// =============================================================================

TEST_CASE("Last-One-Out: promise releases first") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    state->set_value(42);

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);

    // The future has not released, so the state is still alive and readable.
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

    state->release_future();

    // The promise has not released, so the state is still alive and readable.
    REQUIRE_FALSE(state->is_ready());  // promise not released yet
    REQUIRE(state->has_result());

    // Promise releases — and because the future already released, THIS call is
    // the Last-One-Out that deallocates. release_promise() reporting true is the
    // protocol; the discarded return used to hide it.
    const bool deallocated = state->release_promise();
    REQUIRE(deallocated);
    // State is now deallocated — no access!
}

// =============================================================================
// TEST SECTION 5: is_ready() vs has_result() semantics
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
    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
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

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
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

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
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
        std::atomic<int>* role_;

        tracking_resource(std::pmr::memory_resource* up, std::atomic<int>* c, std::atomic<int>* r)
            : upstream_(up), counter_(c), role_(r) {}

        void* do_allocate(std::size_t bytes, std::size_t align) override {
            return upstream_->allocate(bytes, align);
        }

        void do_deallocate(void* p, std::size_t bytes, std::size_t align) override {
            counter_->fetch_add(1, std::memory_order_relaxed);
            role_->store(tls_release_role, std::memory_order_release);
            upstream_->deallocate(p, bytes, align);
        }

        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
            return this == &other;
        }
    };

    std::atomic<int> dealloc_role{0};
    tracking_resource resource(std::pmr::get_default_resource(), &deallocation_count, &dealloc_role);

    // release_promise() reports whether THIS call deallocated the state. The two
    // releases genuinely race, so no per-iteration OUTCOME is assertable -- but
    // the ATTRIBUTION is: whichever side the resource saw deallocate must be the
    // side whose self-report said so. Counting mismatches and asserting zero
    // after the loop is what a release_promise() hard-coded to `return true`
    // fails -- it would claim every iteration the future side actually won.
    int promise_won = 0;
    int attribution_mismatches = 0;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto* state = allocate_shared_state<int>(&resource);

        dealloc_role.store(0, std::memory_order_release);
        std::atomic<bool> promise_claimed{false};

        std::thread t1([state, i, &promise_claimed]() {
            tls_release_role = role_promise;
            state->set_value(int(i));
            promise_claimed.store(state->release_promise(), std::memory_order_release);
        });

        std::thread t2([state]() {
            tls_release_role = role_future;
            // Spin until result is set
            while (!state->has_result()) {
                std::this_thread::yield();
            }
            state->release_future();
        });

        t1.join();
        t2.join();

        const bool claimed = promise_claimed.load(std::memory_order_acquire);
        const int  who     = dealloc_role.load(std::memory_order_acquire);
        if (claimed) {
            ++promise_won;
        }
        if (claimed != (who == role_promise)) {
            ++attribution_mismatches;
        }
    }

    REQUIRE(deallocation_count.load() == NUM_ITERATIONS);
    REQUIRE(attribution_mismatches == 0);
    // Sanity: the promise cannot have won more iterations than there were.
    REQUIRE(promise_won <= NUM_ITERATIONS);
}

TEST_CASE("shared_state: concurrent release_promise and release_future") {
    constexpr int NUM_ITERATIONS = 10000;
    std::atomic<int> deallocation_count{0};

    struct tracking_resource : std::pmr::memory_resource {
        std::pmr::memory_resource* upstream_;
        std::atomic<int>* counter_;
        std::atomic<int>* role_;

        tracking_resource(std::pmr::memory_resource* up, std::atomic<int>* c, std::atomic<int>* r)
            : upstream_(up), counter_(c), role_(r) {}

        void* do_allocate(std::size_t bytes, std::size_t align) override {
            return upstream_->allocate(bytes, align);
        }

        void do_deallocate(void* p, std::size_t bytes, std::size_t align) override {
            counter_->fetch_add(1, std::memory_order_relaxed);
            role_->store(tls_release_role, std::memory_order_release);
            upstream_->deallocate(p, bytes, align);
        }

        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
            return this == &other;
        }
    };

    std::atomic<int> dealloc_role{0};
    tracking_resource resource(std::pmr::get_default_resource(), &deallocation_count, &dealloc_role);

    // Same shape as above: latch release_promise()'s answer, assert after the
    // join -- a Catch2 macro fired from inside a thread is itself a data race.
    int promise_won = 0;
    int attribution_mismatches = 0;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto* state = allocate_shared_state<int>(&resource);
        state->set_value(int(i));

        dealloc_role.store(0, std::memory_order_release);
        std::atomic<bool> promise_claimed{false};

        std::thread t1([state, &promise_claimed]() {
            tls_release_role = role_promise;
            promise_claimed.store(state->release_promise(), std::memory_order_release);
        });

        std::thread t2([state]() {
            tls_release_role = role_future;
            state->release_future();
        });

        t1.join();
        t2.join();

        const bool claimed = promise_claimed.load(std::memory_order_acquire);
        const int  who     = dealloc_role.load(std::memory_order_acquire);
        if (claimed) {
            ++promise_won;
        }
        if (claimed != (who == role_promise)) {
            ++attribution_mismatches;
        }
    }

    // Exactly one deallocation per state — Last-One-Out guarantee
    REQUIRE(deallocation_count.load() == NUM_ITERATIONS);
    REQUIRE(attribution_mismatches == 0);
    // Sanity: the promise cannot have won more iterations than there were.
    REQUIRE(promise_won <= NUM_ITERATIONS);
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

        const bool deallocated = state->release_promise();
        REQUIRE_FALSE(deallocated);
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

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    state->release_future();
}

TEST_CASE("shared_state<vector>: container type") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<std::vector<int>>(resource);

    std::vector<int> test_value = {1, 2, 3, 4, 5};
    state->set_value(std::vector<int>(test_value));

    REQUIRE(state->has_result());
    REQUIRE(state->get_value() == test_value);

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    state->release_future();
}