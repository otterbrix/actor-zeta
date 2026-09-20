#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <vector>
#include <memory>

namespace {

    // Bounded spin, not a driver: the scheduler's workers produce, this thread only
    // waits. Returns nothing on timeout or error rather than asserting, because
    // several call sites run inside consumer threads where a Catch2 macro is a
    // data race (v2 builds an ostringstream); they fold the failure into counters
    // checked after join(). is_ready() alone would not do: it is the
    // promise_released bit, which a promise dying without a value sets too.
    template<typename T>
    std::optional<T> await_from_scheduler(actor_zeta::unique_future<T>& future) {
        constexpr int kSpinCap = 10'000'000;
        for (int i = 0; i < kSpinCap && !future.is_ready(); ++i) {
            std::this_thread::yield();
        }
        if (!future.is_ready() || future.failed()) {
            return std::nullopt;
        }
        return std::move(future).take_ready();
    }

    // Drive `actor` until it stops asking to be rescheduled, or `cap` turns pass.
    // The verdict is the loop condition, returned rather than asserted because
    // producer-thread call sites cannot use a Catch2 macro. Resumed by hand rather
    // than through a scheduler because the route guarded (PR #182) is precisely
    // "an actor resumed from a foreign thread".
    template<typename Actor>
    bool drive(Actor* actor, size_t max_throughput, int cap = 8) {
        for (int i = 0; i < cap; ++i) {
            if (actor->resume(max_throughput).result != actor_zeta::scheduler::resume_result::resume) {
                return true;
            }
        }
        return false;
    }

} // namespace

// The future lives on the consumer thread; the actor runs on another (a
// sharing_scheduler worker or a hand-spawned std::thread).

class cross_thread_worker final : public actor_zeta::basic_actor<cross_thread_worker> {
public:
    explicit cross_thread_worker(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<cross_thread_worker>(resource)
        , processed_{0} {}

    actor_zeta::unique_future<int> compute(int value) {
        ++processed_;
        co_return value * 2;
    }

    // Slower, to widen the window.
    actor_zeta::unique_future<int> compute_slow(int value) {
        // Plain int, not volatile: a compound assignment to a volatile object is
        // deprecated in C++20. The value is co_returned, so the loop still stands.
        int sum = 0;
        for (int i = 0; i < 100; ++i) {
            sum += value;
        }
        ++processed_;
        co_return (sum / 100) * 2;
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

TEST_CASE("cross-thread: basic polling pattern") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<cross_thread_worker>(resource);

    constexpr int NUM_ITERATIONS = 100;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                       &cross_thread_worker::compute, i);

        // The producer signals completion through a test-owned flag, so the
        // consumer blocks instead of spinning.
        std::atomic<bool> done{false};
        std::thread producer([&]() {
            drive(actor.get(), 1);
            done.store(true, std::memory_order_release);
            done.notify_all();
        });

        done.wait(false, std::memory_order_acquire);
        int result = std::move(future).take_ready();
        REQUIRE(result == i * 2);

        producer.join();
    }

    REQUIRE(actor->processed() == NUM_ITERATIONS);
}

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

        std::thread producer([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            drive(actor.get(), 1);
            done.store(true, std::memory_order_release);
            done.notify_all();
        });

        // On its own thread to keep the simultaneous start with the producer.
        std::thread consumer([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            done.wait(false, std::memory_order_acquire);
            int r = std::move(future).take_ready();
            result.store(r, std::memory_order_release);
        });

        start.store(true, std::memory_order_release);

        producer.join();
        consumer.join();

        REQUIRE(result.load() == i * 2);
    }
}

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

        auto result = await_from_scheduler(future);
        REQUIRE(result.has_value());
        REQUIRE(*result == i * 2);
    }

    scheduler->stop();
    REQUIRE(actor->processed() == NUM_ITERATIONS);
}

TEST_CASE("cross-thread: slow computation stress") {
    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 50);
    scheduler->start();

    auto actor = actor_zeta::spawn<cross_thread_worker>(resource);

    constexpr int NUM_ITERATIONS = 100;
    std::atomic<int> completed{0};
    std::atomic<int> correct_results{0};

    std::vector<std::thread> consumers;
    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                       &cross_thread_worker::compute_slow, i);

        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        consumers.emplace_back([fut = std::move(future), i, &completed, &correct_results]() mutable {
            auto result = await_from_scheduler(fut);
            // No REQUIRE in threads: Catch2 is not thread-safe.
            if (result && *result == i * 2) {
                correct_results.fetch_add(1, std::memory_order_relaxed);
            }
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    for (auto& t : consumers) {
        t.join();
    }

    scheduler->stop();

    REQUIRE(completed.load() == NUM_ITERATIONS);
    REQUIRE(correct_results.load() == NUM_ITERATIONS);
    REQUIRE(actor->processed() == NUM_ITERATIONS);
}

TEST_CASE("cross-thread: batch processing") {
    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 100);
    scheduler->start();

    auto actor = actor_zeta::spawn<cross_thread_worker>(resource);

    constexpr int BATCH_SIZE = 20;
    constexpr int NUM_BATCHES = 10;

    for (int batch = 0; batch < NUM_BATCHES; ++batch) {
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

        std::vector<int> results;
        results.reserve(BATCH_SIZE);
        for (auto& f : futures) {
            auto value = await_from_scheduler(f);
            REQUIRE(value.has_value());
            results.push_back(*value);
        }
        for (size_t i = 0; i < static_cast<size_t>(BATCH_SIZE); ++i) {
            int expected = (batch * BATCH_SIZE + static_cast<int>(i)) * 2;
            REQUIRE(results[i] == expected);
        }
    }

    scheduler->stop();
    REQUIRE(actor->processed() == BATCH_SIZE * NUM_BATCHES);
}

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
    std::atomic<int> correct_results{0};
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

                auto result = await_from_scheduler(future);
                int expected = (static_cast<int>(a) * ITERATIONS_PER_ACTOR + i) * 2;
                // No REQUIRE in threads: Catch2 is not thread-safe.
                if (result && *result == expected) {
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

    REQUIRE(total_completed.load() == NUM_ACTORS * ITERATIONS_PER_ACTOR);
    REQUIRE(correct_results.load() == NUM_ACTORS * ITERATIONS_PER_ACTOR);

    for (size_t a = 0; a < static_cast<size_t>(NUM_ACTORS); ++a) {
        REQUIRE(actors[a]->processed() == ITERATIONS_PER_ACTOR);
    }
}

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

        future.detach();
    }

    while (actor->processed() < NUM_ITERATIONS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    scheduler->stop();

    REQUIRE(actor->processed() == NUM_ITERATIONS);
}

TEST_CASE("cross-thread: immediate available") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<cross_thread_worker>(resource);

    constexpr int NUM_ITERATIONS = 100;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                       &cross_thread_worker::compute, i);

        drive(actor.get(), 1);
        REQUIRE(future.is_ready());

        int result = std::move(future).take_ready();
        REQUIRE(result == i * 2);
    }

    REQUIRE(actor->processed() == NUM_ITERATIONS);
}

TEST_CASE("cross-thread: high contention") {
    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(8, 20);
    scheduler->start();

    auto actor = actor_zeta::spawn<cross_thread_worker>(resource);

    constexpr int NUM_THREADS = 8;
    constexpr int ITERATIONS_PER_THREAD = 30;

    std::atomic<int> total_completed{0};
    std::atomic<int> correct_results{0};
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

                auto result = await_from_scheduler(future);
                int expected = (t * ITERATIONS_PER_THREAD + i) * 2;
                // No REQUIRE in threads: Catch2 is not thread-safe.
                if (result && *result == expected) {
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

    REQUIRE(total_completed.load() == NUM_THREADS * ITERATIONS_PER_THREAD);
    REQUIRE(correct_results.load() == NUM_THREADS * ITERATIONS_PER_THREAD);
}

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
            drive(actor.get(), 1);
            producer_done.store(true, std::memory_order_release);
            producer_done.notify_all();
        });

        std::thread consumer([&]() {
            producer_done.wait(false, std::memory_order_acquire);
            int v = std::move(future).take_ready();
            read_value.store(v, std::memory_order_relaxed);
        });

        producer.join();
        consumer.join();

        REQUIRE(read_value.load() == iter * 2);
    }
}
