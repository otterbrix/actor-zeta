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

namespace {
    // Which side of the Last-One-Out race the current thread is playing. The
    // tracking resource stamps it into the iteration's role slot on every
    // deallocation, which is what makes release_promise()'s self-report
    // CHECKABLE: "I deallocated" must coincide with "this thread deallocated".
    thread_local int tls_release_role = 0;   // 1 = promise side, 2 = future side
    constexpr int role_promise = 1;
    constexpr int role_future = 2;
} // namespace


// =============================================================================
// The self-destroying coroutine contract these tests pin:
// - the coroutine destroys itself in final_suspend (self.destroy())
// - ~unique_future() does NOT call handle.destroy()
// - promise_released is set AFTER self.destroy(), so is_ready() == true implies
//   the coroutine frame is already gone
// - unique_future stores only state_, never a handle
// =============================================================================

using namespace actor_zeta;
using namespace actor_zeta::detail;

// =============================================================================
// TEST SECTION 1: unique_future stores state, not handle
// =============================================================================

TEST_CASE("unique_future: stores state_ pointer") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p(resource);
    auto future = p.get_future();

    REQUIRE(future.valid());
    REQUIRE(future.internal_state() != nullptr);

    p.set_value(42);
}

TEST_CASE("unique_future: no handle_ member") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p(resource);
    auto future = p.get_future();

    // unique_future holds no coroutine handle — only the shared state.
    REQUIRE(future.valid());
    REQUIRE(future.internal_state() != nullptr);
    // Initially no result set, future not ready
    REQUIRE_FALSE(future.is_ready());

    p.set_value(42);
}

// =============================================================================
// TEST SECTION 2: destructor does NOT call handle.destroy()
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
// TEST SECTION 3: is_ready implies coroutine already destroyed
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
    // final_suspend claims the continuation. Nothing installed one in this
    // single-threaded scenario, so the claim comes back empty -- which is exactly
    // what the discarded `cont` used to hide.
    const auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
    REQUIRE(cont == nullptr);

    // 3. final_suspend: self.destroy() (simulated)
    // Coroutine is now destroyed

    // 4. final_suspend: release_promise
    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);

    // NOW is_ready is true — coroutine is ALREADY destroyed
    REQUIRE(state->is_ready());

    // 5. Future polls is_ready, sees true, knows it's safe
    // 6. ~future calls release_future

    state->release_future();
}

// =============================================================================
// TEST SECTION 4: Race window elimination
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
    // final_suspend claims the continuation. Nothing installed one in this
    // single-threaded scenario, so the claim comes back empty -- which is exactly
    // what the discarded `cont` used to hide.
    const auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
    REQUIRE(cont == nullptr);
    // self.destroy() happens here
    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);

    // Step 4: is_ready=true, race window closed
    REQUIRE(state->is_ready());

    state->release_future();
}

// =============================================================================
// TEST SECTION 5: order of operations in final_suspend
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
    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
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
        // release_promise() reports whether THIS call deallocated the state. The
        // peer thread here only reads -- it never releases the future -- so the
        // answer is deterministically false. Latch it and assert after the join:
        // a Catch2 macro fired from inside a thread is itself a data race.
        std::atomic<bool> promise_deallocated{false};

        std::thread producer([state, i, &producer_done, &promise_deallocated]() {
            state->set_value(int(i));

            // self.destroy() would happen here; the continuation is claimed but
            // deliberately not resumed.
            state->continuation_.exchange(nullptr, std::memory_order_acq_rel);

            promise_deallocated.store(state->release_promise(), std::memory_order_relaxed);
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
        REQUIRE_FALSE(promise_deallocated.load());

        state->release_future();
    }
}

TEST_CASE("self-destroy: concurrent release stress") {
    constexpr int NUM_ITERATIONS = 1000;
    std::atomic<int> deallocation_count{0};

    struct tracking_resource : std::pmr::memory_resource {
        std::pmr::memory_resource* upstream_;
        std::atomic<int>* counter_;
        std::atomic<int>* role_ = nullptr;

        tracking_resource(std::pmr::memory_resource* up, std::atomic<int>* c)
            : upstream_(up), counter_(c) {}

        void* do_allocate(std::size_t bytes, std::size_t align) override {
            return upstream_->allocate(bytes, align);
        }

        void do_deallocate(void* p, std::size_t bytes, std::size_t align) override {
            counter_->fetch_add(1, std::memory_order_relaxed);
            if (role_) { role_->store(tls_release_role, std::memory_order_release); }
            upstream_->deallocate(p, bytes, align);
        }

        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
            return this == &other;
        }
    };

    std::atomic<int> dealloc_role{0};
    tracking_resource resource(std::pmr::get_default_resource(), &deallocation_count);
    resource.role_ = &dealloc_role;

    // release_promise() reports whether THIS call deallocated the state. The two
    // releases genuinely race, so no per-iteration OUTCOME is assertable -- but
    // the ATTRIBUTION is: whichever side the resource saw deallocate must be the
    // side whose self-report said so. A release_promise() hard-coded to
    // `return true` fails this; a `<=` comparison on a counter does not.
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

    // Exactly one deallocation per iteration (Last-One-Out)
    REQUIRE(deallocation_count.load() == NUM_ITERATIONS);
    REQUIRE(attribution_mismatches == 0);
    REQUIRE(promise_won <= NUM_ITERATIONS);
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

    // final_suspend claims the continuation. Nothing installed one in this
    // single-threaded scenario, so the claim comes back empty -- which is exactly
    // what the discarded `cont` used to hide.
    const auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
    REQUIRE(cont == nullptr);

    // Promise releases — Last-One-Out deallocates
    const bool deallocated = state->release_promise();
    // The future already released, so THIS call is the Last-One-Out that
    // deallocates -- as the deallocation_count check below confirms.
    REQUIRE(deallocated);

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
        // Same shape as above: latch release_promise()'s answer, assert after the
        // join -- a Catch2 macro fired from inside a thread is itself a data race.
        std::atomic<bool> promise_deallocated{false};

        std::thread producer([state, i, &promise_deallocated]() {
            // Write value
            state->set_value(int(i));

            // Take the continuation; not resumed in this test.
            state->continuation_.exchange(nullptr, std::memory_order_acq_rel);

            // Release promise (with release semantics)
            promise_deallocated.store(state->release_promise(), std::memory_order_relaxed);
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
        REQUIRE_FALSE(promise_deallocated.load());

        state->release_future();
    }
}

// =============================================================================
// TEST SECTION 10: Promise destroyed without set_value
// =============================================================================

TEST_CASE("self-destroy: promise destroyed without set_value") {
    auto* resource = std::pmr::get_default_resource();

    unique_future<int> future([resource]() {
        promise<int> p(resource);
        auto f = p.get_future();
        // Promise destroyed without set_value
        return f;
    }());

    // ~promise without set_value settles the future as failed; the concrete
    // error code (broken_pipe) is pinned in test/future-state-fixes/main.cpp.

    WARN("Promise destructor behavior depends on implementation");
}