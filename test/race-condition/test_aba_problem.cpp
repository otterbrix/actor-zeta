#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

template<typename T>
std::pair<bool, T> wait_with_timeout(actor_zeta::unique_future<T>&& future,
                                      std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();
    while (!future.is_ready()) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > timeout) {
            return {false, T{}};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {true, std::move(future).take_ready()};
}

// 60s: TSan is 10-50x slower.
constexpr auto FUTURE_TIMEOUT = std::chrono::seconds(60);

class aba_test_actor final : public actor_zeta::basic_actor<aba_test_actor> {
public:
    explicit aba_test_actor(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<aba_test_actor>(resource) {
    }

    actor_zeta::unique_future<int> process(int value) {
        co_return value * 2;
    }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<aba_test_actor, &aba_test_actor::process>) {
            co_await dispatch(this, &aba_test_actor::process, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &aba_test_actor::process
    >;
};

TEST_CASE("ABA Test 1: Concurrent push_front/take_head stress test") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(resource, 4, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<aba_test_actor>(resource);

    constexpr int NUM_THREADS = 2;   // Minimal for TSAN
    constexpr int MESSAGES_PER_THREAD = 3;  // Minimal for TSAN
    std::atomic<int> total_processed{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, thread_id = t]() {
            for (int i = 0; i < MESSAGES_PER_THREAD; ++i) {
                auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                              &aba_test_actor::process, thread_id * 1000 + i);

                if (needs_sched) {
                    scheduler->enqueue(actor.get());
                }

                // Most futures are dropped early: faster message recycling, more ABA chances.
                if (i % 3 == 0) {
                    auto [success, result] = wait_with_timeout(std::move(future), FUTURE_TIMEOUT);
                    // No REQUIRE here: Catch2 v2 is not thread-safe.
                    actor_zeta::detail::ignore_unused(result);
                    if (!success) {
                        // sentinel, checked after join
                        total_processed.store(-1, std::memory_order_relaxed);
                        return;
                    }
                    total_processed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    total_processed.fetch_add(1, std::memory_order_relaxed);
                }

                if (i % 10 == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    scheduler->stop();

    int processed = total_processed.load();
    if (processed == -1) {
        FAIL("TIMEOUT: Future.get() took longer than 60 seconds - possible deadlock");
    }
    REQUIRE(processed == NUM_THREADS * MESSAGES_PER_THREAD);
}

TEST_CASE("ABA Test 2: Rapid actor creation/destruction stress test") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(resource, 4, 1000);
    scheduler->start();

    constexpr int NUM_ITERATIONS = 5;  // Minimal for TSAN
    constexpr int MESSAGES_PER_ITERATION = 2;  // Minimal for TSAN

    for (int iter = 0; iter < NUM_ITERATIONS; ++iter) {
        auto actor = actor_zeta::spawn<aba_test_actor>(resource);

        std::vector<aba_test_actor::unique_future<int>> futures;
        futures.reserve(MESSAGES_PER_ITERATION);

        for (int i = 0; i < MESSAGES_PER_ITERATION; ++i) {
            auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                          &aba_test_actor::process, i);
            if (needs_sched) {
                scheduler->enqueue(actor.get());
            }
            futures.push_back(std::move(future));
        }

        for (size_t i = 0; i < futures.size(); ++i) {
            auto [success, result] = wait_with_timeout(std::move(futures[i]), FUTURE_TIMEOUT);
            REQUIRE(success);
            REQUIRE(result == static_cast<int>(i) * 2);
        }
    }

    scheduler->stop();
}

TEST_CASE("ABA Test 3: Concurrent enqueue from multiple threads") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(resource, 8, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<aba_test_actor>(resource);

    constexpr int NUM_ENQUEUE_THREADS = 2;  // Minimal for TSAN
    constexpr int ENQUEUES_PER_THREAD = 2;  // Minimal for TSAN
    std::atomic<int> total_sent{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_ENQUEUE_THREADS; ++t) {
        threads.emplace_back([&, thread_id = t]() {
            for (int i = 0; i < ENQUEUES_PER_THREAD; ++i) {
                auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                              &aba_test_actor::process, thread_id * 1000 + i);

                if (needs_sched) {
                    scheduler->enqueue(actor.get());
                }

                total_sent.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    scheduler->stop();

    REQUIRE(total_sent.load() == NUM_ENQUEUE_THREADS * ENQUEUES_PER_THREAD);
}

TEST_CASE("ABA Test 4: Interleaved enqueue/resume stress test") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(resource, 8, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<aba_test_actor>(resource);

    constexpr int NUM_THREADS = 2;  // Minimal for TSAN
    constexpr int OPERATIONS_PER_THREAD = 3;  // Minimal for TSAN
    std::atomic<int> completed{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, thread_id = t]() {
            for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
                auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                              &aba_test_actor::process, thread_id * 1000 + i);
                if (needs_sched) {
                    scheduler->enqueue(actor.get());
                }
                completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    scheduler->stop();

    REQUIRE(completed.load() == NUM_THREADS * OPERATIONS_PER_THREAD);
}
