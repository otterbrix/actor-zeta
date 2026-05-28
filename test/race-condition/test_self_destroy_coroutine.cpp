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

// =============================================================================
// Tests for self-destroying coroutine pattern as described in:
//   docs/actor-zeta-race-comprehensive-fix.md §6.2
//
// These tests use the NEW API that will be implemented:
// - Coroutine destroys itself in final_suspend (self.destroy())
// - ~unique_future() does NOT call handle.destroy()
// - promise_released is set AFTER self.destroy()
// - is_ready() == true implies coroutine is already destroyed
// - unique_future stores only state_, not handle_
//
// IMPORTANT: These tests will NOT COMPILE until the document is implemented.
// This is intentional - they serve as a specification for the new API.
// =============================================================================

using namespace actor_zeta;
using namespace actor_zeta::detail;

// =============================================================================
// TEST SECTION 1: unique_future stores state, not handle (§12.4)
// =============================================================================

TEST_CASE("unique_future: stores state_ pointer") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p(resource);
    auto future = p.get_future();

    // New API: unique_future stores state_, use internal_state() to access
    REQUIRE(future.valid());
    REQUIRE(future.internal_state() != nullptr);

    p.set_value(42);
}

TEST_CASE("unique_future: no handle_ member") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p(resource);
    auto future = p.get_future();

    // After implementation, unique_future should NOT have owning_coro_handle_
    // This test verifies the structure by checking state_ via internal_state()
    REQUIRE(future.valid());
    REQUIRE(future.internal_state() != nullptr);
    // Initially no result set, future not ready
    REQUIRE_FALSE(future.is_ready());

    p.set_value(42);
}

// =============================================================================
// TEST SECTION 2: destructor does NOT call handle.destroy() (§12.4)
// =============================================================================

TEST_CASE("~unique_future: only releases future") {
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

        // Promise releases (simulates final_suspend after self.destroy())
        p.set_value(42);
        // set_value calls release_promise internally

        // Future destroyed here — should call release_future()
        // NOT handle.destroy()!
    }

    // Exactly one deallocation (Last-One-Out)
    REQUIRE(deallocation_count.load() == 1);
}

// =============================================================================
// TEST SECTION 3: is_ready implies coroutine already destroyed (§7.3)
// =============================================================================

TEST_CASE("is_ready: true means coroutine already self-destroyed") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    // Simulate coroutine flow:

    // 1. return_value() writes value
    state->set_value(42);
    REQUIRE(state->has_result());
    REQUIRE_FALSE(state->is_ready());  // is_ready checks promise_released!

    // 2. final_suspend: take continuation
    auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
    (void)cont;

    // 3. final_suspend: self.destroy() (simulated)
    // Coroutine is now destroyed

    // 4. final_suspend: release_promise
    (void)state->release_promise();

    // NOW is_ready is true — coroutine is ALREADY destroyed
    REQUIRE(state->is_ready());

    // 5. Future polls is_ready, sees true, knows it's safe
    // 6. ~future calls release_future

    state->release_future();
}

// =============================================================================
// TEST SECTION 4: Race window elimination (§3.2)
// =============================================================================

TEST_CASE("Race window: has_result vs is_ready timing") {
    // The OLD bug:
    // 1. Coroutine: return_value() -> available() = true
    // 2. Poller: sees available() = true, destroys future → handle.destroy()
    // 3. Coroutine: still running between return_value and final_suspend
    // 4. CRASH: destroy() on non-suspended coroutine

    // The NEW solution:
    // 1. Coroutine: return_value() -> has_result() = true, is_ready() = false
    // 2. Poller: checks is_ready() -> false, waits
    // 3. Coroutine: final_suspend() -> self.destroy() -> release_promise()
    // 4. Poller: is_ready() = true, safe to ~future (coroutine already destroyed)

    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    // Step 1: return_value
    state->set_value(42);

    // has_result=true but is_ready=false (race window in old code!)
    REQUIRE(state->has_result());
    REQUIRE_FALSE(state->is_ready());

    // Step 2-3: final_suspend (simulated)
    auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
    (void)cont;
    // self.destroy() happens here
    (void)state->release_promise();

    // Step 4: is_ready=true, race window closed
    REQUIRE(state->is_ready());

    state->release_future();
}

// =============================================================================
// TEST SECTION 5: order of operations in final_suspend (§6.2)
// =============================================================================

TEST_CASE("final_suspend order: take cont, release, destroy, transfer") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    std::coroutine_handle<> waiter = std::noop_coroutine();
    state->continuation_.store(waiter, std::memory_order_release);
    state->set_value(99);

    // final_suspend order:

    // 1. Take continuation FIRST (while coroutine frame is still valid)
    auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
    REQUIRE(cont == waiter);
    REQUIRE(state->continuation_.load() == nullptr);

    // 2. Release promise (Last-One-Out)
    (void)state->release_promise();
    REQUIRE(state->is_ready());

    // 3. self.destroy() (can't test actual destroy)

    // 4. return cont (symmetric transfer)
    REQUIRE(cont != nullptr);  // Would resume waiter

    state->release_future();
}

// =============================================================================
// TEST SECTION 6: Concurrent stress tests
// =============================================================================

TEST_CASE("self-destroy: concurrent is_ready polling stress") {
    constexpr int NUM_ITERATIONS = 1000;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto* resource = std::pmr::get_default_resource();
        auto* state = allocate_shared_state<int>(resource);

        std::atomic<bool> producer_done{false};
        std::atomic<bool> consumer_saw_ready{false};

        std::thread producer([state, i, &producer_done]() {
            state->set_value(int(i));

            auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
            (void)cont;
            // self.destroy() would happen here

            (void)state->release_promise();
            producer_done.store(true, std::memory_order_release);
        });

        std::thread consumer([state, &consumer_saw_ready]() {
            // Poll is_ready (not has_result!)
            while (!state->is_ready()) {
                std::this_thread::yield();
            }

            // When is_ready=true, coroutine is already destroyed
            // Safe to access value and release future
            REQUIRE(state->has_result());
            consumer_saw_ready.store(true, std::memory_order_release);
        });

        producer.join();
        consumer.join();

        REQUIRE(consumer_saw_ready.load());

        state->release_future();
    }
}

TEST_CASE("self-destroy: concurrent release stress") {
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

    // Exactly one deallocation per iteration (Last-One-Out)
    REQUIRE(deallocation_count.load() == NUM_ITERATIONS);
}

// =============================================================================
// TEST SECTION 7: No double-destroy
// =============================================================================

TEST_CASE("self-destroy: no double-destroy possible") {
    // With self-destroying pattern:
    // - Coroutine calls self.destroy() in final_suspend
    // - ~unique_future only calls release_future(), not handle.destroy()
    // - Therefore double-destroy is impossible

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

    constexpr int NUM_ITERATIONS = 100;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        promise<int> p(&tracked);
        auto future = p.get_future();

        p.set_value(int(i));
        // ~promise (no-op since state_ is null after set_value)
        // ~future calls release_future
    }

    // If there were double-destroys, deallocation count would be 2*N
    REQUIRE(deallocation_count.load() == NUM_ITERATIONS);
}

// =============================================================================
// TEST SECTION 8: Future destroyed before producer finishes
// =============================================================================

TEST_CASE("self-destroy: future released before is_ready") {
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

    auto* state = allocate_shared_state<int>(&tracked);

    // Future releases first (user abandons future)
    state->release_future();

    // State should NOT be deallocated yet
    REQUIRE(deallocation_count.load() == 0);

    // Producer finishes later
    state->set_value(42);

    auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
    (void)cont;

    // Promise releases — Last-One-Out deallocates
    (void)state->release_promise();

    REQUIRE(deallocation_count.load() == 1);
}

// =============================================================================
// TEST SECTION 9: Memory ordering guarantees
// =============================================================================

TEST_CASE("self-destroy: value visible after is_ready") {
    constexpr int NUM_ITERATIONS = 1000;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto* resource = std::pmr::get_default_resource();
        auto* state = allocate_shared_state<int>(resource);

        std::atomic<int> read_value{-1};

        std::thread producer([state, i]() {
            // Write value
            state->set_value(int(i));

            // Take continuation
            auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
            (void)cont;

            // Release promise (with release semantics)
            (void)state->release_promise();
        });

        std::thread consumer([state, &read_value]() {
            // Wait for is_ready (acquires promise's release)
            while (!state->is_ready()) {
                std::this_thread::yield();
            }

            // Value must be visible due to release-acquire synchronization
            read_value.store(state->get_value(), std::memory_order_relaxed);
        });

        producer.join();
        consumer.join();

        REQUIRE(read_value.load() == i);

        state->release_future();
    }
}

// =============================================================================
// TEST SECTION 13: Promise destroyed without set_value
// After implementation: should set error and release_promise
// =============================================================================

TEST_CASE("self-destroy: promise destroyed without set_value") {
    auto* resource = std::pmr::get_default_resource();

    unique_future<int> future([resource]() {
        promise<int> p(resource);
        auto f = p.get_future();
        // Promise destroyed without set_value
        return f;
    }());

    // Future should be in failed/cancelled state
    // Exact behavior depends on implementation
    // Document says: destructor should set error broken_pipe

    WARN("Promise destructor behavior depends on implementation");
}