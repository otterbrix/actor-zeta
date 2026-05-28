#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <memory>

// =============================================================================
// Test for verifying the race condition FIX between available() and final_suspend()
//
// With the NEW architecture:
// - available() returns true only AFTER release_promise() in final_suspend
// - Coroutine destroys ITSELF in final_suspend (self-destroying pattern)
// - Future destructor only releases state, does NOT destroy coroutine handle
//
// These tests verify that destroying a future after available()==true is SAFE.
// =============================================================================

namespace {
// Drive a cross-actor future to ready without taking — these tests destroy the
// future untaken to exercise destroy-on-ready safety.
template<typename Fut, typename Sched, typename A1, typename A2>
inline void drive_until_ready(Fut& fut, Sched& sched, A1* a1, A2* a2) {
    while (!fut.is_ready()) {
        sched->enqueue(a1);
        sched->enqueue(a2);
        std::this_thread::yield();
    }
}
} // namespace

// Sentinel to track destructions
class destruction_tracker {
public:
    static constexpr uint64_t MAGIC = 0xCAFEBABEDEADBEEFULL;

    explicit destruction_tracker(int id)
        : magic_(MAGIC)
        , id_(id) {
        alive_.fetch_add(1, std::memory_order_relaxed);
    }

    ~destruction_tracker() {
        if (magic_ != MAGIC) {
            double_destroy_.fetch_add(1, std::memory_order_relaxed);
        }
        magic_ = 0;
        alive_.fetch_sub(1, std::memory_order_relaxed);
    }

    destruction_tracker(const destruction_tracker&) = delete;
    destruction_tracker& operator=(const destruction_tracker&) = delete;

    int id() const { return id_; }

    static int alive_count() { return alive_.load(std::memory_order_acquire); }
    static int double_destroy_count() { return double_destroy_.load(std::memory_order_acquire); }
    static void reset() {
        alive_.store(0, std::memory_order_release);
        double_destroy_.store(0, std::memory_order_release);
    }

private:
    uint64_t magic_;
    int id_;
    static std::atomic<int> alive_;
    static std::atomic<int> double_destroy_;
};

std::atomic<int> destruction_tracker::alive_{0};
std::atomic<int> destruction_tracker::double_destroy_{0};

// Forward declaration
class worker_actor;

// =============================================================================
// Worker Actor - executes work and returns result
// =============================================================================
class worker_actor final : public actor_zeta::basic_actor<worker_actor> {
public:
    explicit worker_actor(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<worker_actor>(resource) {}

    // Coroutine that does work with local variable
    actor_zeta::unique_future<int> execute(int value) {
        // Local variable - will be destroyed when coroutine completes
        destruction_tracker tracker(value);

        // Simulate work
        volatile int sum = 0;
        for (int i = 0; i < 100; ++i) {
            sum += tracker.id();
        }

        co_return static_cast<int>(sum);
    }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<worker_actor, &worker_actor::execute>) {
            co_await dispatch(this, &worker_actor::execute, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&worker_actor::execute>;
};

// =============================================================================
// Dispatcher Actor - sends work to workers and awaits results
// =============================================================================
class dispatcher_actor final : public actor_zeta::basic_actor<dispatcher_actor> {
public:
    explicit dispatcher_actor(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<dispatcher_actor>(resource)
        , completed_(0)
        , worker_address_(actor_zeta::address_t::empty_address()) {}

    void set_worker(const actor_zeta::address_t& addr) {
        worker_address_ = addr;
    }

    // Coroutine that chains to worker
    actor_zeta::unique_future<int> process(int value) {
        destruction_tracker tracker(value + 1000);

        // Send to worker and await result
        auto [_, future] = actor_zeta::send(worker_address_, &worker_actor::execute, value);

        // co_await - suspend here until worker's execute() completes
        int result = co_await std::move(future);

        completed_.fetch_add(1, std::memory_order_relaxed);
        co_return result + tracker.id();
    }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<dispatcher_actor, &dispatcher_actor::process>) {
            co_await dispatch(this, &dispatcher_actor::process, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&dispatcher_actor::process>;

    int completed() const { return completed_.load(std::memory_order_acquire); }

private:
    std::atomic<int> completed_;
    actor_zeta::address_t worker_address_;
};

// =============================================================================
// Test 1: Basic chaining - verify available() is safe for destruction
// =============================================================================
TEST_CASE("available race chaining: basic chain is safe") {
    destruction_tracker::reset();

    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 100);
    scheduler->start();

    auto worker = actor_zeta::spawn<worker_actor>(resource);
    auto dispatcher = actor_zeta::spawn<dispatcher_actor>(resource);
    dispatcher->set_worker(worker->address());

    constexpr int NUM_ITERATIONS = 100;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto [needs_sched, future] = actor_zeta::send(dispatcher.get(),
                                       &dispatcher_actor::process, i);

        if (needs_sched) {
            scheduler->enqueue(dispatcher.get());
        }

        // Schedule worker once (it will be scheduled again via message passing)
        scheduler->enqueue(worker.get());

        // Wait for future to become available (file-local driver: re-enqueues both
        // actors to handle cross-actor messaging; does not take_ready — see helper).
        drive_until_ready(future, scheduler, dispatcher.get(), worker.get());

        // Destroy future - this should be SAFE with new architecture
        // because is_ready() == true means release_promise() was called
        // which happens AFTER self.destroy() in final_suspend
        { auto temp = std::move(future); }
    }

    // Let pending work complete
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    scheduler->stop();

    INFO("Completed: " << dispatcher->completed());
    INFO("Double destroys: " << destruction_tracker::double_destroy_count());

    REQUIRE(destruction_tracker::double_destroy_count() == 0);
    REQUIRE(dispatcher->completed() == NUM_ITERATIONS);
}

// =============================================================================
// Test 2: poll_pending pattern - sequential polling
// =============================================================================
TEST_CASE("available race chaining: poll_pending pattern") {
    destruction_tracker::reset();

    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 50);
    scheduler->start();

    auto worker = actor_zeta::spawn<worker_actor>(resource);
    auto dispatcher = actor_zeta::spawn<dispatcher_actor>(resource);
    dispatcher->set_worker(worker->address());

    constexpr int BATCH_SIZE = 10;
    constexpr int NUM_BATCHES = 5;

    for (int batch = 0; batch < NUM_BATCHES; ++batch) {
        // Accumulate pending futures
        std::vector<actor_zeta::unique_future<int>> pending;
        pending.reserve(BATCH_SIZE);

        for (int i = 0; i < BATCH_SIZE; ++i) {
            auto [needs_sched, future] = actor_zeta::send(dispatcher.get(),
                                           &dispatcher_actor::process, batch * BATCH_SIZE + i);

            if (needs_sched) {
                scheduler->enqueue(dispatcher.get());
            }
            scheduler->enqueue(worker.get());

            pending.push_back(std::move(future));
        }

        // poll_pending() pattern - drain completed futures
        while (!pending.empty()) {
            scheduler->enqueue(dispatcher.get());
            scheduler->enqueue(worker.get());
            for (auto it = pending.begin(); it != pending.end();) {
                if (it->is_ready()) {
                    // Safe to destroy - is_ready() is true only after full completion
                    it = pending.erase(it);
                } else {
                    ++it;
                }
            }
            std::this_thread::yield();
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    scheduler->stop();

    INFO("Completed: " << dispatcher->completed());
    INFO("Double destroys: " << destruction_tracker::double_destroy_count());

    REQUIRE(destruction_tracker::double_destroy_count() == 0);
    REQUIRE(dispatcher->completed() == BATCH_SIZE * NUM_BATCHES);
}

// =============================================================================
// Test 3: Concurrent senders with sequential actor processing
// =============================================================================
TEST_CASE("available race chaining: concurrent senders") {
    destruction_tracker::reset();

    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 100);
    scheduler->start();

    auto worker = actor_zeta::spawn<worker_actor>(resource);
    auto dispatcher = actor_zeta::spawn<dispatcher_actor>(resource);
    dispatcher->set_worker(worker->address());

    constexpr int OPERATIONS_PER_THREAD = 20;
    constexpr int NUM_THREADS = 4;
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
                auto [needs_sched, future] = actor_zeta::send(dispatcher.get(),
                                               &dispatcher_actor::process,
                                               t * OPERATIONS_PER_THREAD + i);

                if (needs_sched) {
                    scheduler->enqueue(dispatcher.get());
                }
                scheduler->enqueue(worker.get());

                // Wait for completion (file-local driver — see helper above).
                drive_until_ready(future, scheduler, dispatcher.get(), worker.get());

                // Safe to destroy
                { auto temp = std::move(future); }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    scheduler->stop();

    INFO("Completed: " << dispatcher->completed());
    INFO("Double destroys: " << destruction_tracker::double_destroy_count());

    REQUIRE(destruction_tracker::double_destroy_count() == 0);
    REQUIRE(dispatcher->completed() == NUM_THREADS * OPERATIONS_PER_THREAD);
}

// =============================================================================
// Test 4: Verify is_ready vs has_result distinction
// This tests the core fix - is_ready should only be true after release_promise
// =============================================================================
TEST_CASE("available race chaining: is_ready vs has_result") {
    auto* resource = std::pmr::get_default_resource();

    // Direct state test
    auto* state = actor_zeta::detail::allocate_shared_state<int>(resource);

    // Initially both false
    REQUIRE_FALSE(state->has_result());
    REQUIRE_FALSE(state->is_ready());

    // After set_value, has_result is true but is_ready is false
    state->set_value(42);
    REQUIRE(state->has_result());
    REQUIRE_FALSE(state->is_ready());  // Promise not released!

    // After release_promise, is_ready becomes true
    (void)state->release_promise();
    REQUIRE(state->is_ready());

    // Cleanup
    state->release_future();
}