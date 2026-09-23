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

template<typename T>
bool wait_available_with_timeout(actor_zeta::unique_future<T>& future,
                                  std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();
    while (!future.is_ready()) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > timeout) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// 60s: TSan is 10-50x slower.
constexpr auto FUTURE_TIMEOUT = std::chrono::seconds(60);

class refcount_test_actor final : public actor_zeta::basic_actor<refcount_test_actor> {
public:
    explicit refcount_test_actor(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<refcount_test_actor>(resource) {
    }

    actor_zeta::unique_future<int> echo(int value) {
        co_return value;
    }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<refcount_test_actor, &refcount_test_actor::echo>) {
            co_await dispatch(this, &refcount_test_actor::echo, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &refcount_test_actor::echo
    >;
};

TEST_CASE("Refcount Test 2.1: Concurrent actor + future release") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<refcount_test_actor>(resource);

    constexpr int NUM_ITERATIONS = 1000;
    std::atomic<int> completed{0};

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                      &refcount_test_actor::echo, i);

        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        // Releasing the future on another thread races the worker releasing the promise.
        std::thread destroyer([fut = std::move(future)]() mutable {
            if (std::rand() % 2 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
        });

        destroyer.join();
        completed.fetch_add(1, std::memory_order_relaxed);
    }

    scheduler->stop();

    // The REQUIRE only proves the loop ran; the real check is a clean ASan/TSan run.
    REQUIRE(completed.load() == NUM_ITERATIONS);
}

TEST_CASE("Refcount Test 2.2: Stress test with 1000 concurrent futures") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<refcount_test_actor>(resource);

    constexpr int NUM_THREADS = 4;
    constexpr int FUTURES_PER_THREAD = 250;
    std::atomic<int> total_completed{0};
    std::atomic<bool> start_flag{false};

    std::vector<std::thread> workers;
    workers.reserve(NUM_THREADS);

    for (int t = 0; t < NUM_THREADS; ++t) {
        workers.emplace_back([&, thread_id = t]() {
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < FUTURES_PER_THREAD; ++i) {
                int value = thread_id * FUTURES_PER_THREAD + i;

                auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                              &refcount_test_actor::echo, value);

                if (needs_sched) {
                    scheduler->enqueue(actor.get());
                }

                int pattern = std::rand() % 4;
                switch (pattern) {
                    case 0:
                        break;
                    case 1:
                        std::this_thread::sleep_for(std::chrono::microseconds(1));
                        break;
                    case 2:
                        std::this_thread::yield();
                        break;
                    case 3:
                        if (future.is_ready()) {
                            auto result = std::move(future).take_ready();
                            actor_zeta::detail::ignore_unused(result);
                        }
                        break;
                }

                total_completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start_flag.store(true, std::memory_order_release);

    for (auto& worker : workers) {
        worker.join();
    }

    scheduler->stop();

    REQUIRE(total_completed.load() == NUM_THREADS * FUTURES_PER_THREAD);
}

TEST_CASE("Refcount Test 2.3: Refcount correctness under various destruction patterns") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<refcount_test_actor>(resource);

    // Must be divisible by 4: Scenario 4 splits it across 4 threads.
    constexpr int ITERATIONS = 48;

    SECTION("Scenario 1: Orphan futures") {
        for (int i = 0; i < ITERATIONS; ++i) {
            {
                auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                              &refcount_test_actor::echo, i);
                if (needs_sched) {
                    scheduler->enqueue(actor.get());
                }
            }
        }
        // let the orphans get processed before stop()
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    SECTION("Scenario 2: Consumed futures") {
        for (int i = 0; i < ITERATIONS; ++i) {
            auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                          &refcount_test_actor::echo, i);
            if (needs_sched) {
                scheduler->enqueue(actor.get());
            }

            auto [success, result] = wait_with_timeout(std::move(future), FUTURE_TIMEOUT);
            REQUIRE(success);
            REQUIRE(result == i);
        }
    }

    SECTION("Scenario 3: Ready but not consumed") {
        for (int i = 0; i < ITERATIONS; ++i) {
            auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                          &refcount_test_actor::echo, i);
            if (needs_sched) {
                scheduler->enqueue(actor.get());
            }

            bool ready = wait_available_with_timeout(future, FUTURE_TIMEOUT);
            REQUIRE(ready);
        }
    }

    SECTION("Scenario 4: Interleaved multi-threaded") {
        std::atomic<int> completed{0};
        std::vector<std::thread> threads;

        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, pattern = t]() {
                for (int i = 0; i < ITERATIONS / 4; ++i) {
                    auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                                  &refcount_test_actor::echo, i);
                    if (needs_sched) {
                        scheduler->enqueue(actor.get());
                    }

                    switch (pattern) {
                        case 0:
                            break;
                        case 1:
                            if (future.is_ready()) {
                                actor_zeta::detail::ignore_unused(std::move(future).take_ready());
                            }
                            break;
                        case 2:
                            actor_zeta::detail::ignore_unused(wait_available_with_timeout(future, FUTURE_TIMEOUT));
                            break;
                        case 3:
                            std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 10));
                            break;
                    }
                    completed.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        REQUIRE(completed.load() == ITERATIONS);
    }

    scheduler->stop();
}
