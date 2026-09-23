#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/detail/future.hpp>
#include <actor-zeta/detail/shared_state.hpp>

#include <atomic>
#include <thread>
#include <vector>
#include <memory_resource>

namespace {
    // Which side of the Last-One-Out race the current thread is playing. The
    // tracking resource stamps it into the iteration's role slot on every
    // deallocation, which is what makes release_promise()'s self-report
    // CHECKABLE: "I deallocated" must coincide with "this thread deallocated".
    thread_local int tls_release_role = 0;   // 1 = promise side, 2 = future side
    constexpr int role_promise = 1;
    constexpr int role_future = 2;
} // namespace


using namespace actor_zeta;
using namespace actor_zeta::detail;

TEST_CASE("Issue #1: is_ready vs has_result distinction", "[race][availability]") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    REQUIRE_FALSE(state->has_result());
    REQUIRE_FALSE(state->is_ready());

    state->set_value(42);
    REQUIRE(state->has_result());
    REQUIRE_FALSE(state->is_ready());  // is_ready() is promise_released, not the value

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    REQUIRE(state->is_ready());

    state->release_future();
}

TEST_CASE("Issue #1: void specialization", "[race][void]") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<void>(resource);

    REQUIRE_FALSE(state->has_result());
    REQUIRE_FALSE(state->is_ready());

    state->set_value();
    REQUIRE(state->has_result());
    REQUIRE_FALSE(state->is_ready());

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    REQUIRE(state->is_ready());

    state->release_future();
}

// The atomic flag word orders the error_code write against every reader.
TEST_CASE("Issue #2: error state is atomic", "[error][atomic]") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    auto ec = std::make_error_code(std::errc::invalid_argument);
    state->set_error(ec);

    REQUIRE(state->has_error());
    REQUIRE(state->has_result());  // error counts as result
    REQUIRE(state->get_error() == ec);

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    state->release_future();
}

TEST_CASE("Issue #3: error state handling is consistent for int type", "[error][consistency]") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p(resource);
    auto f = p.get_future();  // before the error
    p.error(std::make_error_code(std::errc::invalid_argument));

    REQUIRE(f.failed());
    REQUIRE(f.error() == std::make_error_code(std::errc::invalid_argument));
}

TEST_CASE("Issue #3: error state handling is consistent for void type", "[error][consistency]") {
    auto* resource = std::pmr::get_default_resource();

    promise<void> p(resource);
    auto f = p.get_future();  // before the error
    p.error(std::make_error_code(std::errc::invalid_argument));

    REQUIRE(f.failed());
    REQUIRE(f.error() == std::make_error_code(std::errc::invalid_argument));
}

TEST_CASE("Issue #3: cancelled state handling is consistent for int type", "[cancel][consistency]") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p(resource);
    unique_future<int> f = p.get_future();
    // Cancellation is the promise's error channel.
    p.error(std::make_error_code(std::errc::operation_canceled));

    REQUIRE(f.failed());
    REQUIRE(f.error() == std::make_error_code(std::errc::operation_canceled));
}

TEST_CASE("Issue #3: cancelled state handling is consistent for void type", "[cancel][consistency]") {
    auto* resource = std::pmr::get_default_resource();

    promise<void> p(resource);
    unique_future<void> f = p.get_future();
    p.error(std::make_error_code(std::errc::operation_canceled));

    REQUIRE(f.failed());
    REQUIRE(f.error() == std::make_error_code(std::errc::operation_canceled));
}

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
        const bool deallocated = state->release_promise();
        REQUIRE_FALSE(deallocated);
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
        const bool deallocated = state->release_promise();
        // The future already released, so THIS call is the Last-One-Out (deallocation_count confirms).
        REQUIRE(deallocated);
        REQUIRE(deallocation_count.load() == 1);  // Last one out deallocates
    }
}

TEST_CASE("Issue #5: operator= does not overwrite error state", "[operator=][error]") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p1(resource);
    unique_future<int> f1 = p1.get_future();
    p1.error(std::make_error_code(std::errc::invalid_argument));

    promise<int> p2(resource);
    unique_future<int> f2 = p2.get_future();
    p2.set_value(42);

    f1 = std::move(f2);

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

        f1 = std::move(f2);

        // f1's old state: promise and future both released.
        REQUIRE(deallocation_count.load() == 1);
    }

    REQUIRE(deallocation_count.load() == 2);
}

TEST_CASE("Concurrent: promise and future release race", "[concurrent][memory]") {
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

    // Attribution check (why: race-condition/test_shared_state.cpp); a `return true`
    // release_promise() fails it, a `<=` comparison on a counter would not.
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

TEST_CASE("Concurrent: is_ready polling is safe", "[concurrent][polling]") {
    constexpr int NUM_ITERATIONS = 1000;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto* resource = std::pmr::get_default_resource();
        auto* state = allocate_shared_state<int>(resource);

        std::atomic<bool> producer_done{false};
        std::atomic<int> read_value{-1};
        // release_promise() reports whether THIS call deallocated the state. The
        // peer thread here only reads -- it never releases the future -- so the
        // answer is deterministically false. Latch it and assert after the join:
        // a Catch2 macro fired from inside a thread is itself a data race.
        std::atomic<bool> promise_deallocated{false};

        std::thread producer([state, i, &producer_done, &promise_deallocated]() {
            state->set_value(int(i));
            promise_deallocated.store(state->release_promise(), std::memory_order_relaxed);
            producer_done.store(true, std::memory_order_release);
        });

        std::thread consumer([state, &read_value]() {
            while (!state->is_ready()) {
                std::this_thread::yield();
            }
            read_value.store(state->get_value(), std::memory_order_relaxed);
        });

        producer.join();
        consumer.join();

        REQUIRE(read_value.load() == i);
        REQUIRE_FALSE(promise_deallocated.load());
        state->release_future();
    }
}

// Once is_ready() returns true (acquire on flags_), a relaxed store the producer
// made before set_value() must be visible: the release/acquire chain is the subject.
TEST_CASE("Concurrent: is_ready acquire synchronizes side effect on shared_state",
          "[concurrent][polling][memory-ordering]") {
    constexpr int NUM_ITERATIONS = 1000;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto* resource = std::pmr::get_default_resource();
        auto* state = allocate_shared_state<int>(resource);

        std::atomic<int> side_effect{0};
        std::atomic<int> read_side_effect{-1};

        // Latched, asserted after the join: a Catch2 macro inside a thread races.
        std::atomic<bool> promise_deallocated{false};

        std::thread writer([state, &side_effect, &promise_deallocated]() {
            side_effect.store(42, std::memory_order_relaxed);
            state->set_value(100);              // release on flags_
            promise_deallocated.store(state->release_promise(),   // acq_rel on flags_
                                      std::memory_order_relaxed);
        });

        std::thread reader([state, &side_effect, &read_side_effect]() {
            while (!state->is_ready()) {
                std::this_thread::yield();
            }
            read_side_effect.store(side_effect.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
        });

        writer.join();
        reader.join();

        REQUIRE(read_side_effect.load(std::memory_order_relaxed) == 42);
        REQUIRE_FALSE(promise_deallocated.load());
        state->release_future();
    }
}

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
        return f;
    }());

    REQUIRE(future.is_ready());
    REQUIRE(future.failed());
    REQUIRE(future.error() == std::make_error_code(std::errc::broken_pipe));
}

TEST_CASE("Integration: move semantics", "[integration][move]") {
    auto* resource = std::pmr::get_default_resource();

    promise<int> p1(resource);
    auto f1 = p1.get_future();
    auto* original_state = f1.internal_state();

    promise<int> p2(std::move(p1));
    REQUIRE_FALSE(p1.valid());
    REQUIRE(p2.valid());

    unique_future<int> f2(std::move(f1));
    REQUIRE_FALSE(f1.valid());
    REQUIRE(f2.valid());
    REQUIRE(f2.internal_state() == original_state);

    p2.set_value(123);
    REQUIRE(f2.is_ready());
    REQUIRE(std::move(f2).take_ready() == 123);
}
// SETTLED-OUTCOME / I1 -- producer totality: an acquire load that observes
// promise_released observes a result bit in the same load, `is_ready() =>
// has_result()`. Without the repair in release_promise() a promise released
// without an outcome would answer is_ready()==true, failed()==false, with nothing
// to take -- and under NDEBUG nothing stands between that and unset storage.

TEST_CASE("SETTLED-OUTCOME: release without an outcome is repaired, not published raw",
          "[invariant][settled-outcome]") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    REQUIRE_FALSE(state->is_ready());
    REQUIRE_FALSE(state->has_result());

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);

    REQUIRE(state->is_ready());
    REQUIRE(state->has_result());   // I1: ready implies an outcome exists
    REQUIRE(state->has_error());    // and it is an error, not a phantom value
    REQUIRE(state->get_error() == std::make_error_code(std::errc::state_not_recoverable));

    state->release_future();
}

TEST_CASE("SETTLED-OUTCOME: totality holds for the void specialization too",
          "[invariant][settled-outcome]") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<void>(resource);

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);

    REQUIRE(state->is_ready());
    REQUIRE(state->has_result());
    REQUIRE(state->has_error());

    state->release_future();
}

TEST_CASE("SETTLED-OUTCOME: a real value is never overwritten by the repair",
          "[invariant][settled-outcome]") {
    auto* resource = std::pmr::get_default_resource();
    auto* state = allocate_shared_state<int>(resource);

    state->set_value(42);
    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);

    REQUIRE(state->is_ready());
    REQUIRE(state->has_result());
    REQUIRE_FALSE(state->has_error());   // the repair must not fire here
    REQUIRE(state->get_value() == 42);

    state->release_future();
}

TEST_CASE("SETTLED-OUTCOME: holds_value distinguishes the four outcomes",
          "[invariant][settled-outcome]") {
    auto* resource = std::pmr::get_default_resource();

    SECTION("pending: released? no. value? no.") {
        auto* state = allocate_shared_state<int>(resource);
        REQUIRE_FALSE(state->holds_value());          // nothing written yet
        const bool d = state->release_promise();
        REQUIRE_FALSE(d);
        REQUIRE(state->is_ready());
        REQUIRE_FALSE(state->holds_value());          // repaired to an error, not a value
        state->release_future();
    }

    SECTION("value present, then consumed -- I2 makes the second take visible") {
        auto* state = allocate_shared_state<int>(resource);
        state->set_value(7);
        REQUIRE(state->holds_value());

        REQUIRE(state->take_value() == 7);
        // The value bit is NOT cleared (flags are monotonic); `consumed` is now set, which is
        // how a second extraction becomes detectable instead of reading moved-from storage.
        REQUIRE(state->has_result());
        REQUIRE_FALSE(state->has_error());
        REQUIRE_FALSE(state->holds_value());

        const bool d = state->release_promise();
        REQUIRE_FALSE(d);
        state->release_future();
    }

    SECTION("error: ready, but never extractable") {
        auto* state = allocate_shared_state<int>(resource);
        state->set_error(std::make_error_code(std::errc::operation_canceled));
        const bool d = state->release_promise();
        REQUIRE_FALSE(d);
        REQUIRE(state->is_ready());
        REQUIRE(state->has_error());
        REQUIRE_FALSE(state->holds_value());
        state->release_future();
    }
}
