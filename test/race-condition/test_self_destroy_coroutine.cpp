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
    // Which side of the Last-One-Out race this thread plays. The tracking resource
    // stamps it on every deallocation, making release_promise()'s self-report CHECKABLE.
    thread_local int tls_release_role = 0;   // 1 = promise side, 2 = future side
    constexpr int role_promise = 1;
    constexpr int role_future = 2;
} // namespace


// shared_state driven by hand through final_awaiter::await_suspend's steps -- claim
// the continuation, then release_promise() -- to pin that is_ready() means
// promise_released: set after the value and after the claim, never before. Frame
// ownership is pinned in test/external-drive, not here.

using namespace actor_zeta;
using namespace actor_zeta::detail;

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

    REQUIRE(future.valid());
    REQUIRE(future.internal_state() != nullptr);
    REQUIRE_FALSE(future.is_ready());

    p.set_value(42);
}

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

        p.set_value(42);
    }

    // Exactly one deallocation: promise released by set_value, future by scope.
    REQUIRE(deallocation_count.load() == 1);
}

TEST_CASE("is_ready: true means coroutine already self-destroyed") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    state->set_value(42);
    REQUIRE(state->has_result());
    REQUIRE_FALSE(state->is_ready());  // is_ready() is promise_released, not the value

    // Nothing installed a continuation, so the claim must come back empty.
    const auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
    REQUIRE(cont == nullptr);

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);

    REQUIRE(state->is_ready());

    state->release_future();
}

TEST_CASE("Race window: has_result vs is_ready timing") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    state->set_value(42);

    // A poller gating on has_result() would act here; is_ready() waits for release_promise().
    REQUIRE(state->has_result());
    REQUIRE_FALSE(state->is_ready());

    const auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
    REQUIRE(cont == nullptr);
    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);

    REQUIRE(state->is_ready());

    state->release_future();
}

TEST_CASE("final_suspend order: take cont, release, destroy, transfer") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    std::coroutine_handle<> waiter = std::noop_coroutine();
    state->continuation_.store(waiter, std::memory_order_release);
    state->set_value(99);

    // The continuation is claimed FIRST, then the promise released.
    auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
    REQUIRE(cont == waiter);
    REQUIRE(state->continuation_.load() == nullptr);

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    REQUIRE(state->is_ready());

    REQUIRE(cont != nullptr);

    state->release_future();
}

TEST_CASE("self-destroy: concurrent is_ready polling stress") {
    constexpr int NUM_ITERATIONS = 1000;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto* resource = std::pmr::get_default_resource();
        auto* state = allocate_shared_state<int>(resource);

        std::atomic<bool> producer_done{false};
        std::atomic<bool> consumer_saw_ready{false};
        // release_promise() reports whether THIS call deallocated; the peer only reads, so
        // it is deterministically false. Latched: a Catch2 macro inside a thread is itself a race.
        std::atomic<bool> promise_deallocated{false};

        std::thread producer([state, i, &producer_done, &promise_deallocated]() {
            state->set_value(int(i));

            // The continuation is claimed but deliberately not resumed.
            state->continuation_.exchange(nullptr, std::memory_order_acq_rel);

            promise_deallocated.store(state->release_promise(), std::memory_order_relaxed);
            producer_done.store(true, std::memory_order_release);
        });

        std::thread consumer([state, &consumer_saw_ready]() {
            while (!state->is_ready()) {
                std::this_thread::yield();
            }

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

    // The two releases genuinely race, so no per-iteration OUTCOME is assertable --
    // but ATTRIBUTION is: the side the resource saw deallocate must be the side whose
    // self-report said so. `return true` fails this; a `<=` on a counter would not.
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

    REQUIRE(deallocation_count.load() == NUM_ITERATIONS);
    REQUIRE(attribution_mismatches == 0);
    REQUIRE(promise_won <= NUM_ITERATIONS);
}

TEST_CASE("self-destroy: no double-destroy possible") {
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
    }

    // A double release would show up as 2*N.
    REQUIRE(deallocation_count.load() == NUM_ITERATIONS);
}

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

    state->release_future();

    REQUIRE(deallocation_count.load() == 0);

    state->set_value(42);

    const auto cont = state->continuation_.exchange(nullptr, std::memory_order_acq_rel);
    REQUIRE(cont == nullptr);

    // The future already released, so THIS call is the Last-One-Out.
    const bool deallocated = state->release_promise();
    REQUIRE(deallocated);

    REQUIRE(deallocation_count.load() == 1);
}

TEST_CASE("self-destroy: value visible after is_ready") {
    constexpr int NUM_ITERATIONS = 1000;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto* resource = std::pmr::get_default_resource();
        auto* state = allocate_shared_state<int>(resource);

        std::atomic<int> read_value{-1};
        // Same shape as above: latched, asserted after the join.
        std::atomic<bool> promise_deallocated{false};

        std::thread producer([state, i, &promise_deallocated]() {
            state->set_value(int(i));

            // Claimed, not resumed.
            state->continuation_.exchange(nullptr, std::memory_order_acq_rel);

            promise_deallocated.store(state->release_promise(), std::memory_order_relaxed);
        });

        std::thread consumer([state, &read_value]() {
            while (!state->is_ready()) {
                std::this_thread::yield();
            }

            // The value must be visible: release_promise() releases, is_ready() acquires.
            read_value.store(state->get_value(), std::memory_order_relaxed);
        });

        producer.join();
        consumer.join();

        REQUIRE(read_value.load() == i);
        REQUIRE_FALSE(promise_deallocated.load());

        state->release_future();
    }
}

TEST_CASE("self-destroy: promise destroyed without set_value") {
    auto* resource = std::pmr::get_default_resource();

    unique_future<int> future([resource]() {
        promise<int> p(resource);
        auto f = p.get_future();
        return f;
    }());

    // The outcome (failed, broken_pipe) is pinned in test/future-state-fixes; nothing asserted here.
    WARN("Promise destructor behavior depends on implementation");
}