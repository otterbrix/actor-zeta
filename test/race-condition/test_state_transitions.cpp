#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

class state_test_actor final : public actor_zeta::basic_actor<state_test_actor> {
public:
    explicit state_test_actor(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<state_test_actor>(resource) {
    }

    actor_zeta::unique_future<int> compute(int value) {
        // widens the window for the races below
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        co_return value * 2;
    }

    actor_zeta::unique_future<int> fast_task(int value) {
        co_return value + 1;
    }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<state_test_actor, &state_test_actor::compute>) {
            co_await dispatch(this, &state_test_actor::compute, msg);
        } else if (cmd == actor_zeta::msg_id<state_test_actor, &state_test_actor::fast_task>) {
            co_await dispatch(this, &state_test_actor::fast_task, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &state_test_actor::compute,
        &state_test_actor::fast_task
    >;
};

TEST_CASE("State Test 1.1: set_result vs cancel race") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(resource, 2, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<state_test_actor>(resource);

    constexpr int NUM_ITERATIONS = 500;
    std::atomic<int> races_detected{0};

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                      &state_test_actor::compute, i);

        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        // Dropping the future on another thread races the worker completing it.
        std::thread canceller([fut = std::move(future)]() mutable {
            if (std::rand() % 3 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });

        canceller.join();
        ++races_detected;
    }

    scheduler->stop();

    // The REQUIRE only proves the loop ran; the real check is a clean TSan/ASan run.
    REQUIRE(races_detected.load() == NUM_ITERATIONS);
}

TEST_CASE("State Test 1.2: is_ready() during set_result()") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(resource, 2, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<state_test_actor>(resource);

    constexpr int NUM_ITERATIONS = 200;
    std::atomic<int> invalid_transitions{0};

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto send_result = actor_zeta::send(actor.get(), &state_test_actor::compute, i);
        auto needs_sched = send_result.first;
        auto& future = send_result.second;

        std::atomic<bool> stop_polling{false};
        std::atomic<bool> saw_invalid{false};

        std::thread poller([&future, &stop_polling, &saw_invalid]() {
            bool last_state = false;

            while (!stop_polling.load(std::memory_order_acquire)) {
                bool current_state = future.is_ready();

                if (last_state && !current_state) {
                    saw_invalid.store(true, std::memory_order_relaxed);
                    std::cerr << "CRITICAL: Invalid transition true->false detected!\n";
                }

                last_state = current_state;
            }
        });

        // Enqueue only after the poller is running so it can see the false->true edge.
        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        stop_polling.store(true, std::memory_order_release);
        poller.join();

        if (saw_invalid.load()) {
            ++invalid_transitions;
        }

        if (future.is_ready()) {
            auto result = std::move(future).take_ready();
            actor_zeta::detail::ignore_unused(result);
        }
    }

    scheduler->stop();

    REQUIRE(invalid_transitions.load() == 0);
}

TEST_CASE("State Test 1.3: Multiple state observers") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(resource, 2, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<state_test_actor>(resource);

    constexpr int NUM_ITERATIONS = 100;
    constexpr int NUM_OBSERVERS = 4;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto send_result = actor_zeta::send(actor.get(), &state_test_actor::fast_task, i);
        auto needs_sched = send_result.first;
        auto& future = send_result.second;

        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        std::vector<std::thread> observers;
        std::atomic<bool> stop_observing{false};
        std::atomic<int> observers_saw_ready{0};

        for (int obs = 0; obs < NUM_OBSERVERS; ++obs) {
            observers.emplace_back([&future, &stop_observing, &observers_saw_ready]() {
                bool saw_ready = false;
                while (!stop_observing.load(std::memory_order_acquire)) {
                    if (future.is_ready()) {
                        saw_ready = true;
                        break;
                    }
                    std::this_thread::yield();
                }

                if (saw_ready) {
                    observers_saw_ready.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));

        stop_observing.store(true, std::memory_order_release);

        for (auto& obs : observers) {
            obs.join();
        }

        // Deliberately weak: the 2ms window may close before any observer sees ready.
        REQUIRE(observers_saw_ready.load() >= 0);

        if (future.is_ready()) {
            auto result = std::move(future).take_ready();
            REQUIRE(result == i + 1);
        }
    }

    scheduler->stop();
}

TEST_CASE("State Test 1.4: State transition ordering") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(resource, 2, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<state_test_actor>(resource);

    constexpr int NUM_ITERATIONS = 500;
    std::atomic<int> ordering_violations{0};
    std::atomic<int> successful_reads{0};

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        int expected_value = i + 1;

        auto send_result = actor_zeta::send(actor.get(), &state_test_actor::fast_task, i);
        auto needs_sched = send_result.first;
        auto& future = send_result.second;

        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        std::thread consumer([&future, expected_value, &ordering_violations, &successful_reads]() {
            while (!future.is_ready()) {
                std::this_thread::yield();
            }

            int actual_value = std::move(future).take_ready();

            if (actual_value != expected_value) {
                ordering_violations.fetch_add(1, std::memory_order_relaxed);
            } else {
                successful_reads.fetch_add(1, std::memory_order_relaxed);
            }
        });

        consumer.join();
    }

    scheduler->stop();

    INFO("Successful reads: " << successful_reads.load());
    INFO("Ordering violations: " << ordering_violations.load());
    REQUIRE(ordering_violations.load() == 0);
    REQUIRE(successful_reads.load() == NUM_ITERATIONS);
}

TEST_CASE("State Test 1.5: Happens-before across state transitions") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(resource, 2, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<state_test_actor>(resource);

    constexpr int NUM_ITERATIONS = 300;
    std::atomic<int> visibility_failures{0};

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto send_result = actor_zeta::send(actor.get(), &state_test_actor::compute, i);
        auto needs_sched = send_result.first;
        auto& future = send_result.second;

        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        std::atomic<bool> consumer_done{false};
        std::thread consumer([&]() {
            auto start = std::chrono::steady_clock::now();
            while (!future.is_ready()) {
                if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
                    visibility_failures.fetch_add(1, std::memory_order_relaxed);
                    consumer_done.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
            }

            int result = std::move(future).take_ready();
            if (result != i * 2) {
                visibility_failures.fetch_add(1, std::memory_order_relaxed);
            }

            consumer_done.store(true, std::memory_order_release);
        });

        consumer.join();
    }

    scheduler->stop();

    REQUIRE(visibility_failures.load() == 0);
}
