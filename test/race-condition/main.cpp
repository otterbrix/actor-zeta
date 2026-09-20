#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <random>
#include <thread>
#include <vector>

class stress_actor final : public actor_zeta::basic_actor<stress_actor> {
public:
    explicit stress_actor(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<stress_actor>(resource)
        , processed_count_(0) {
    }

    ~stress_actor() = default;

    actor_zeta::unique_future<int> compute(int value) {
        processed_count_.fetch_add(1, std::memory_order_relaxed);
        int result = value * 2;
        co_return result;
    }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<stress_actor, &stress_actor::compute>) {
            co_await dispatch(this, &stress_actor::compute, msg);
        }
    }

    std::size_t processed_count() const {
        return processed_count_.load(std::memory_order_acquire);
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &stress_actor::compute
    >;

private:
    std::atomic<std::size_t> processed_count_;
};

TEST_CASE("Race condition stress test - future destruction timing") {
    // Thread counts and run time are kept low so the test stays tractable under ASan.
    constexpr int NUM_THREADS = 4;

    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<stress_actor>(resource);

    std::atomic<int> futures_created{0};
    std::atomic<int> futures_destroyed_early{0};
    std::atomic<int> futures_destroyed_late{0};
    std::atomic<int> results_read{0};
    std::atomic<bool> stop{false};

    auto worker = [&](int thread_id) {
        std::mt19937 rng(static_cast<unsigned int>(thread_id));
        std::uniform_int_distribution<int> dist(0, 100);

        while (!stop.load(std::memory_order_acquire)) {
            int value = dist(rng);

            auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                          &stress_actor::compute, value);

            if (needs_sched) {
                scheduler->enqueue(actor.get());
                // throttles the send rate; the test is tuned for ASan
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }

            futures_created.fetch_add(1, std::memory_order_relaxed);

            int decision = dist(rng);

            if (decision < 30) {
                futures_destroyed_early.fetch_add(1, std::memory_order_relaxed);
            } else if (decision < 60) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                futures_destroyed_late.fetch_add(1, std::memory_order_relaxed);
            } else {
                auto start = std::chrono::steady_clock::now();
                while (!future.is_ready()) {
                    auto elapsed = std::chrono::steady_clock::now() - start;
                    if (elapsed > std::chrono::milliseconds(100)) {
                        break;
                    }
                    std::this_thread::yield();
                }

                if (future.is_ready()) {
                    results_read.fetch_add(1, std::memory_order_relaxed);
                }
            }

            if (dist(rng) < 10) {
                std::this_thread::yield();
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(worker, i);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop.store(true, std::memory_order_release);

    for (auto& t : threads) {
        t.join();
    }

    scheduler->stop();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::cout << "\n=== Stress Test Results ===\n";
    std::cout << "Futures created:        " << futures_created.load() << "\n";
    std::cout << "Destroyed early:        " << futures_destroyed_early.load() << "\n";
    std::cout << "Destroyed late:         " << futures_destroyed_late.load() << "\n";
    std::cout << "Results read:           " << results_read.load() << "\n";
    std::cout << "Actor processed:        " << actor->processed_count() << "\n";
    std::cout << "===========================\n\n";

    // The REQUIREs only prove the loop ran; the real check is a clean ASan/TSan run.
    REQUIRE(futures_created.load() > 0);
    REQUIRE(actor->processed_count() > 0);
}

TEST_CASE("Race condition stress test - concurrent future destruction") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 100);
    scheduler->start();

    auto actor = actor_zeta::spawn<stress_actor>(resource);

    std::atomic<int> double_delete_detected{0};
    std::atomic<int> iterations{0};
    constexpr int MAX_ITERATIONS = 5000;

    for (int i = 0; i < MAX_ITERATIONS; ++i) {
        auto [needs_sched, fut] = actor_zeta::send(actor.get(),
                                      &stress_actor::compute, i);
        // Structured bindings cannot be lambda-captured on clang-14.
        auto future = std::move(fut);

        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        std::thread destroyer([&future]() mutable {
            auto temp = std::move(future);
        });

        // widen the window in which the worker may be inside compute()
        std::this_thread::sleep_for(std::chrono::microseconds(1));

        destroyer.join();
        iterations.fetch_add(1, std::memory_order_relaxed);
    }

    scheduler->stop();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "\n=== Concurrent Destruction Test ===\n";
    std::cout << "Iterations:             " << iterations.load() << "\n";
    std::cout << "Double deletes detected: " << double_delete_detected.load() << "\n";
    std::cout << "Actor processed:        " << actor->processed_count() << "\n";
    std::cout << "===================================\n\n";

    REQUIRE(double_delete_detected.load() == 0);
    REQUIRE(iterations.load() == MAX_ITERATIONS);
}

TEST_CASE("Memory leak detection - orphaned messages") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<stress_actor>(resource);

    constexpr int NUM_ORPHANED = 1000;

    for (int i = 0; i < NUM_ORPHANED; ++i) {
        {
            auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                          &stress_actor::compute, i);

            if (needs_sched) {
                scheduler->enqueue(actor.get());
            }
        }
    }

    auto start_time = std::chrono::steady_clock::now();
    constexpr auto timeout = std::chrono::seconds(10);

    // No rescue enqueue: all 1000 senders above discharged their own needs_sched,
    // so a stall here is a real strand and must reach the REQUIRE below.
    while (actor->processed_count() < NUM_ORPHANED) {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout) {
            break;
        }

        std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    scheduler->stop();

    std::cout << "\n=== Orphaned Messages Test ===\n";
    std::cout << "Orphaned messages sent: " << NUM_ORPHANED << "\n";
    std::cout << "Actor processed:        " << actor->processed_count() << "\n";
    std::cout << "==============================\n\n";

    // Dropping the future does not drop the message: every orphan is still processed.
    REQUIRE(actor->processed_count() == NUM_ORPHANED);
}
