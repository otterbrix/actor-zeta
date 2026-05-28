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

// Cross-thread stress tests: future lives on the consumer (main/test) thread;
// the actor (producer) runs on another thread (a sharing_scheduler worker or
// a manually-spawned std::thread).

// Simple worker actor for testing
class cross_thread_worker final : public actor_zeta::basic_actor<cross_thread_worker> {
public:
    explicit cross_thread_worker(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<cross_thread_worker>(resource)
        , processed_{0} {}

    // Simple computation
    actor_zeta::unique_future<int> compute(int value) {
        ++processed_;
        co_return value * 2;
    }

    // Computation with delay (to increase race window)
    actor_zeta::unique_future<int> compute_slow(int value) {
        // Simulate work
        volatile int sum = 0;
        for (int i = 0; i < 100; ++i) {
            sum += value;
        }
        ++processed_;
        co_return static_cast<int>(sum / 100) * 2;
    }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<cross_thread_worker, &cross_thread_worker::compute>) {
            co_await dispatch(this, &cross_thread_worker::compute, msg);
        } else if (cmd == actor_zeta::msg_id<cross_thread_worker, &cross_thread_worker::compute_slow>) {
            co_await dispatch(this, &cross_thread_worker::compute_slow, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &cross_thread_worker::compute,
        &cross_thread_worker::compute_slow>;

    int processed() const { return processed_.load(std::memory_order_acquire); }

private:
    std::atomic<int> processed_;
};

// =============================================================================
// Test 1: Cross-thread polling pattern (basic)
// =============================================================================
TEST_CASE("cross-thread: basic polling pattern") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<cross_thread_worker>(resource);

    constexpr int NUM_ITERATIONS = 100;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                       &cross_thread_worker::compute, i);

        // Producer thread (PR #182 resume-crosses-threads path is preserved).
        // It signals completion via a test-owned flag so the consumer waits
        // without busy-spinning (producer is test-controlled and can notify).
        std::atomic<bool> done{false};
        std::thread producer([&]() {
            actor->resume(1);
            done.store(true, std::memory_order_release);
            done.notify_all();
        });

        // Consumer on this thread: block (no spin) until the producer signals, then take.
        done.wait(false, std::memory_order_acquire);
        int result = std::move(future).take_ready();
        REQUIRE(result == i * 2);

        producer.join();
    }

    REQUIRE(actor->processed() == NUM_ITERATIONS);
}

// =============================================================================
// Test 2: Cross-thread polling with concurrent start
// =============================================================================
TEST_CASE("cross-thread: concurrent start polling") {
    constexpr int NUM_ITERATIONS = 500;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto* resource = std::pmr::get_default_resource();
        auto actor = actor_zeta::spawn<cross_thread_worker>(resource);

        auto send_result = actor_zeta::send(actor.get(),&cross_thread_worker::compute, i);
        auto& future = send_result.second;

        std::atomic<bool> start{false};
        std::atomic<bool> done{false};
        std::atomic<int> result{-1};

        // Producer thread
        std::thread producer([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            actor->resume(1);
            done.store(true, std::memory_order_release);
            done.notify_all();
        });

        // Consumer thread (kept on its own thread to preserve the simultaneous-
        // start race with the producer). The start barrier stays a spin; once
        // running, block (no spin) on the producer's completion flag, then take.
        std::thread consumer([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            done.wait(false, std::memory_order_acquire);
            int r = std::move(future).take_ready();
            result.store(r, std::memory_order_release);
        });

        // Start both threads simultaneously
        start.store(true, std::memory_order_release);

        producer.join();
        consumer.join();

        REQUIRE(result.load() == i * 2);
    }
}

// =============================================================================
// Test 3: Cross-thread with scheduler
// =============================================================================
TEST_CASE("cross-thread: polling with scheduler") {
    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 100);
    scheduler->start();

    auto actor = actor_zeta::spawn<cross_thread_worker>(resource);

    constexpr int NUM_ITERATIONS = 200;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                       &cross_thread_worker::compute, i);

        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        // Producer is the scheduler's worker threads; drive on this thread via
        // run_until_complete with a yield pump (nothing to pump locally).
        int result = actor_zeta::run_until_complete(future, [] { std::this_thread::yield(); });
        REQUIRE(result == i * 2);
    }

    scheduler->stop();
    REQUIRE(actor->processed() == NUM_ITERATIONS);
}

// =============================================================================
// Test 4: Cross-thread stress with slow computation
// =============================================================================
TEST_CASE("cross-thread: slow computation stress") {
    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 50);
    scheduler->start();

    auto actor = actor_zeta::spawn<cross_thread_worker>(resource);

    constexpr int NUM_ITERATIONS = 100;
    std::atomic<int> completed{0};
    std::atomic<int> correct_results{0};  // Track correct results atomically

    std::vector<std::thread> consumers;
    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                       &cross_thread_worker::compute_slow, i);

        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        consumers.emplace_back([fut = std::move(future), i, &completed, &correct_results]() mutable {
            // Consumer kept on its own thread; the scheduler worker threads
            // produce, so drive via run_until_complete with a yield pump.
            int result = actor_zeta::run_until_complete(fut, [] { std::this_thread::yield(); });
            // Don't use REQUIRE in threads - Catch2 is not thread-safe
            if (result == i * 2) {
                correct_results.fetch_add(1, std::memory_order_relaxed);
            }
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    for (auto& t : consumers) {
        t.join();
    }

    scheduler->stop();

    // Check results after all threads have joined (thread-safe)
    REQUIRE(completed.load() == NUM_ITERATIONS);
    REQUIRE(correct_results.load() == NUM_ITERATIONS);
    REQUIRE(actor->processed() == NUM_ITERATIONS);
}

// =============================================================================
// Test 5: Cross-thread batch processing
// =============================================================================
TEST_CASE("cross-thread: batch processing") {
    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 100);
    scheduler->start();

    auto actor = actor_zeta::spawn<cross_thread_worker>(resource);

    constexpr int BATCH_SIZE = 20;
    constexpr int NUM_BATCHES = 10;

    for (int batch = 0; batch < NUM_BATCHES; ++batch) {
        // Create batch of futures
        std::vector<actor_zeta::unique_future<int>> futures;
        futures.reserve(BATCH_SIZE);

        for (int i = 0; i < BATCH_SIZE; ++i) {
            auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                           &cross_thread_worker::compute,
                                           batch * BATCH_SIZE + i);

            if (needs_sched) {
                scheduler->enqueue(actor.get());
            }

            futures.push_back(std::move(future));
        }

        // Wait for all futures in batch. The scheduler's worker threads are the
        // producers; the consumer just polls each future then takes its value.
        std::vector<int> results;
        results.reserve(BATCH_SIZE);
        for (auto& f : futures) {
            results.push_back(actor_zeta::run_until_complete(f, [] { std::this_thread::yield(); }));
        }
        for (size_t i = 0; i < static_cast<size_t>(BATCH_SIZE); ++i) {
            int expected = (batch * BATCH_SIZE + static_cast<int>(i)) * 2;
            REQUIRE(results[i] == expected);
        }
    }

    scheduler->stop();
    REQUIRE(actor->processed() == BATCH_SIZE * NUM_BATCHES);
}

// =============================================================================
// Test 6: Cross-thread with multiple actors
// =============================================================================
TEST_CASE("cross-thread: multiple actors") {
    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 50);
    scheduler->start();

    constexpr int NUM_ACTORS = 4;
    constexpr int ITERATIONS_PER_ACTOR = 50;

    std::vector<std::unique_ptr<cross_thread_worker, actor_zeta::pmr::deleter_t>> actors;
    for (int a = 0; a < NUM_ACTORS; ++a) {
        actors.push_back(actor_zeta::spawn<cross_thread_worker>(resource));
    }

    std::atomic<int> total_completed{0};
    std::atomic<int> correct_results{0};  // Track correct results atomically
    std::vector<std::thread> threads;

    for (size_t a = 0; a < static_cast<size_t>(NUM_ACTORS); ++a) {
        threads.emplace_back([&, a]() {
            for (int i = 0; i < ITERATIONS_PER_ACTOR; ++i) {
                auto [needs_sched, future] = actor_zeta::send(actors[a].get(),
                                               &cross_thread_worker::compute,
                                               static_cast<int>(a) * ITERATIONS_PER_ACTOR + i);

                if (needs_sched) {
                    scheduler->enqueue(actors[a].get());
                }

                // Consumer kept on its own thread; the scheduler worker threads
                // produce, so drive via run_until_complete with a yield pump.
                int result = actor_zeta::run_until_complete(future, [] { std::this_thread::yield(); });
                int expected = (static_cast<int>(a) * ITERATIONS_PER_ACTOR + i) * 2;
                // Don't use REQUIRE in threads - Catch2 is not thread-safe
                if (result == expected) {
                    correct_results.fetch_add(1, std::memory_order_relaxed);
                }

                total_completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    scheduler->stop();

    // Check results after all threads have joined (thread-safe)
    REQUIRE(total_completed.load() == NUM_ACTORS * ITERATIONS_PER_ACTOR);
    REQUIRE(correct_results.load() == NUM_ACTORS * ITERATIONS_PER_ACTOR);

    for (size_t a = 0; a < static_cast<size_t>(NUM_ACTORS); ++a) {
        REQUIRE(actors[a]->processed() == ITERATIONS_PER_ACTOR);
    }
}

// =============================================================================
// Test 7: Cross-thread fire-and-forget (detach)
// =============================================================================
TEST_CASE("cross-thread: fire-and-forget") {
    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 100);
    scheduler->start();

    auto actor = actor_zeta::spawn<cross_thread_worker>(resource);

    constexpr int NUM_ITERATIONS = 100;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                       &cross_thread_worker::compute, i);

        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        // Fire and forget - don't wait for result
        future.detach();
    }

    // Wait for all to process
    while (actor->processed() < NUM_ITERATIONS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    scheduler->stop();

    REQUIRE(actor->processed() == NUM_ITERATIONS);
}

// =============================================================================
// Test 8: Cross-thread immediate available (result already set)
// =============================================================================
TEST_CASE("cross-thread: immediate available") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<cross_thread_worker>(resource);

    constexpr int NUM_ITERATIONS = 100;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                       &cross_thread_worker::compute, i);

        // Process immediately in same thread
        actor->resume(1);

        // Should be ready immediately
        REQUIRE(future.is_ready());

        int result = std::move(future).take_ready();
        REQUIRE(result == i * 2);
    }

    REQUIRE(actor->processed() == NUM_ITERATIONS);
}

// =============================================================================
// Test 9: Cross-thread high contention
// =============================================================================
TEST_CASE("cross-thread: high contention") {
    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(8, 20);
    scheduler->start();

    auto actor = actor_zeta::spawn<cross_thread_worker>(resource);

    constexpr int NUM_THREADS = 8;
    constexpr int ITERATIONS_PER_THREAD = 30;

    std::atomic<int> total_completed{0};
    std::atomic<int> correct_results{0};  // Track correct results atomically
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < ITERATIONS_PER_THREAD; ++i) {
                auto send_result = actor_zeta::send(actor.get(),
                                               &cross_thread_worker::compute,
                                               t * ITERATIONS_PER_THREAD + i);
                auto needs_sched = send_result.first;
                auto& future = send_result.second;

                if (needs_sched) {
                    scheduler->enqueue(actor.get());
                }

                // Consumer kept on its own thread (high-contention path); the
                // scheduler worker threads produce, so drive via run_until_complete.
                int result = actor_zeta::run_until_complete(future, [] { std::this_thread::yield(); });
                int expected = (t * ITERATIONS_PER_THREAD + i) * 2;
                // Don't use REQUIRE in threads - Catch2 is not thread-safe
                if (result == expected) {
                    correct_results.fetch_add(1, std::memory_order_relaxed);
                }
                total_completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    scheduler->stop();

    // Check results after all threads have joined (thread-safe)
    REQUIRE(total_completed.load() == NUM_THREADS * ITERATIONS_PER_THREAD);
    REQUIRE(correct_results.load() == NUM_THREADS * ITERATIONS_PER_THREAD);
}

// =============================================================================
// Test 10: Memory ordering verification
// =============================================================================
TEST_CASE("cross-thread: memory ordering") {
    constexpr int NUM_ITERATIONS = 500;

    for (int iter = 0; iter < NUM_ITERATIONS; ++iter) {
        auto* resource = std::pmr::get_default_resource();
        auto actor = actor_zeta::spawn<cross_thread_worker>(resource);

        auto send_result = actor_zeta::send(actor.get(),&cross_thread_worker::compute, iter);
        auto& future = send_result.second;

        std::atomic<int> read_value{-1};
        std::atomic<bool> producer_done{false};

        std::thread producer([&]() {
            actor->resume(1);
            producer_done.store(true, std::memory_order_release);
            producer_done.notify_all();
        });

        std::thread consumer([&]() {
            // Consumer kept on its own thread (producer resumes the actor on a
            // separate thread - PR #182 path). Block (no spin) on the producer's
            // completion flag (release/acquire), then take.
            producer_done.wait(false, std::memory_order_acquire);
            int v = std::move(future).take_ready();
            read_value.store(v, std::memory_order_relaxed);
        });

        producer.join();
        consumer.join();

        REQUIRE(read_value.load() == iter * 2);
    }
}
