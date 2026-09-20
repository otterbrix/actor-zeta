#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <thread>
#include <vector>

class refcount_stress_actor final : public actor_zeta::basic_actor<refcount_stress_actor> {
public:
    explicit refcount_stress_actor(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<refcount_stress_actor>(resource)
        , value_{0} {
    }

    actor_zeta::unique_future<void> increment(int delta) {
        value_.fetch_add(delta, std::memory_order_relaxed);
        co_return;
    }

    actor_zeta::unique_future<int> get_value() const {
        co_return value_.load(std::memory_order_relaxed);
    }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<refcount_stress_actor, &refcount_stress_actor::increment>) {
            co_await dispatch(this, &refcount_stress_actor::increment, msg);
        } else if (cmd == actor_zeta::msg_id<refcount_stress_actor, &refcount_stress_actor::get_value>) {
            co_await dispatch(this, &refcount_stress_actor::get_value, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &refcount_stress_actor::increment,
        &refcount_stress_actor::get_value
    >;

private:
    mutable std::atomic<int> value_;
};

// Polls the future, re-enqueueing the actor every ~1ms in case it went idle with
// the message still queued.
template<typename T, typename Actor>
T smart_get(typename Actor::template unique_future<T>&& future,
            Actor* actor,
            actor_zeta::scheduler::sharing_scheduler* scheduler) {
    constexpr auto timeout = std::chrono::seconds(10);
    auto start_time = std::chrono::steady_clock::now();

    int stall_iterations = 0;
    constexpr int MAX_STALL = 10;

    while (!future.is_ready()) {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout) {
            // on timeout take_ready() asserts -- that is the failure mode
            break;
        }

        ++stall_iterations;

        if (stall_iterations >= MAX_STALL) {
            scheduler->enqueue(actor);
            stall_iterations = 0;
        }

        std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    return std::move(future).take_ready();
}

TEST_CASE("Refcount Stress 1: Concurrent future creation/destruction") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(8, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<refcount_stress_actor>(resource);

    constexpr int NUM_THREADS = 2;   // Minimal for TSAN
    constexpr int OPERATIONS_PER_THREAD = 3;  // Minimal for TSAN
    std::atomic<int> completed{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
                auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                              &refcount_stress_actor::increment, 1);

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

TEST_CASE("Refcount Stress 2: Future move and copy operations") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<refcount_stress_actor>(resource);

    constexpr int NUM_ITERATIONS = 3;  // Minimal for TSAN
    std::atomic<int> move_count{0};

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto [needs_sched, future1] = actor_zeta::send(actor.get(),
                                        &refcount_stress_actor::get_value);

        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        auto future2 = std::move(future1);
        auto future3 = std::move(future2);

        int result = smart_get<int>(std::move(future3), actor.get(), scheduler.get());
        actor_zeta::detail::ignore_unused(result);

        move_count.fetch_add(1, std::memory_order_relaxed);
    }

    scheduler->stop();
    REQUIRE(move_count.load() == NUM_ITERATIONS);
}

TEST_CASE("Refcount Stress 3: Concurrent message enqueue and future get") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<refcount_stress_actor>(resource);

    constexpr int NUM_ITERATIONS = 3;  // Minimal for TSAN
    std::atomic<int> results_received{0};

    std::vector<std::thread> threads;

    threads.emplace_back([&]() {
        for (int i = 0; i < NUM_ITERATIONS; ++i) {
            auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                          &refcount_stress_actor::get_value);
            if (needs_sched) {
                scheduler->enqueue(actor.get());
            }

            int result = smart_get<int>(std::move(future), actor.get(), scheduler.get());
            actor_zeta::detail::ignore_unused(result);
            results_received.fetch_add(1, std::memory_order_relaxed);
        }
    });

    threads.emplace_back([&]() {
        for (int i = 0; i < NUM_ITERATIONS; ++i) {
            auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                          &refcount_stress_actor::increment, 1);
            if (needs_sched) {
                scheduler->enqueue(actor.get());
            }
        }
    });

    threads.emplace_back([&]() {
        for (int i = 0; i < NUM_ITERATIONS; ++i) {
            {
                auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                              &refcount_stress_actor::increment, -1);
                if (needs_sched) {
                    scheduler->enqueue(actor.get());
                }
            }
        }
    });

    for (auto& thread : threads) {
        thread.join();
    }

    scheduler->stop();
    REQUIRE(results_received.load() == NUM_ITERATIONS);
}

TEST_CASE("Refcount Stress 4: Mixed operations stress test") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(8, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<refcount_stress_actor>(resource);

    constexpr int NUM_THREADS = 2;   // Minimal for TSAN
    constexpr int OPERATIONS_PER_THREAD = 3;  // Minimal for TSAN
    std::atomic<int> operation_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, thread_id = t]() {
            for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
                int op = (thread_id * OPERATIONS_PER_THREAD + i) % 5;

                switch (op) {
                    case 0: {
                        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                                      &refcount_stress_actor::increment, 1);
                        if (needs_sched) {
                            scheduler->enqueue(actor.get());
                        }
                        break;
                    }
                    case 1: {
                        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                                      &refcount_stress_actor::get_value);
                        if (needs_sched) {
                            scheduler->enqueue(actor.get());
                        }
                        int result = smart_get<int>(std::move(future), actor.get(), scheduler.get());
                        actor_zeta::detail::ignore_unused(result);
                        break;
                    }
                    case 2: {
                        auto [needs_sched, future1] = actor_zeta::send(actor.get(),
                                                       &refcount_stress_actor::increment, -1);
                        if (needs_sched) {
                            scheduler->enqueue(actor.get());
                        }
                        auto future2 = std::move(future1);
                        break;
                    }
                    case 3: {
                        auto [needs_sched, future1] = actor_zeta::send(actor.get(),
                                                       &refcount_stress_actor::get_value);
                        if (needs_sched) {
                            scheduler->enqueue(actor.get());
                        }
                        auto future2 = std::move(future1);
                        int result = smart_get<int>(std::move(future2), actor.get(), scheduler.get());
                        actor_zeta::detail::ignore_unused(result);
                        break;
                    }
                    case 4: {
                        auto [needs_sched, future1] = actor_zeta::send(actor.get(),
                                                       &refcount_stress_actor::increment, 1);
                        if (needs_sched) {
                            scheduler->enqueue(actor.get());
                        }
                        auto future2 = std::move(future1);
                        auto future3 = std::move(future2);
                        break;
                    }
                }

                operation_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    scheduler->stop();
    REQUIRE(operation_count.load() == NUM_THREADS * OPERATIONS_PER_THREAD);
}

TEST_CASE("Refcount Stress 5: Actor destruction with pending messages") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 1000);
    scheduler->start();

    constexpr int NUM_ITERATIONS = 3;  // Minimal for TSAN

    for (int iter = 0; iter < NUM_ITERATIONS; ++iter) {
        auto actor = actor_zeta::spawn<refcount_stress_actor>(resource);

        constexpr int MESSAGES = 2;  // Minimal for TSAN
        std::vector<refcount_stress_actor::unique_future<int>> futures;
        futures.reserve(MESSAGES);

        for (int i = 0; i < MESSAGES; ++i) {
            auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                          &refcount_stress_actor::get_value);
            if (needs_sched) {
                scheduler->enqueue(actor.get());
            }
            futures.push_back(std::move(future));
        }

        for (auto& future : futures) {
            int result = smart_get<int>(std::move(future), actor.get(), scheduler.get());
            actor_zeta::detail::ignore_unused(result);
        }
    }

    scheduler->stop();
}
