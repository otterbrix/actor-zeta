#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <thread>
#include <vector>

class shutdown_test_actor final : public actor_zeta::basic_actor<shutdown_test_actor> {
public:
    explicit shutdown_test_actor(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<shutdown_test_actor>(resource) {
    }

    ~shutdown_test_actor() = default;

    actor_zeta::unique_future<int> slow_task(int value) {
        // slow enough that stop() lands mid-backlog
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        co_return value * 2;
    }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<shutdown_test_actor, &shutdown_test_actor::slow_task>) {
            co_await dispatch(this, &shutdown_test_actor::slow_task, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &shutdown_test_actor::slow_task
    >;
};

TEST_CASE("Shutdown Test 4.1: Actor destroyed with pending futures (safe pattern)") {
    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);
    scheduler->start();

    std::vector<actor_zeta::unique_future<int>> futures;

    // Function scope: the actor must outlive scheduler->stop() below.
    auto actor = actor_zeta::spawn<shutdown_test_actor>(resource);

    constexpr int NUM_MESSAGES = 10;
    for (int i = 0; i < NUM_MESSAGES; ++i) {
        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                      &shutdown_test_actor::slow_task, i);

        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        futures.push_back(std::move(future));
    }

    // let part of the backlog get processed before stopping
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // stop() joins the workers; only after that may the actor die.
    scheduler->stop();

    int successful = 0;
    for (auto& future : futures) {
        if (future.is_ready()) {
            if (!future.failed()) {
                auto result = std::move(future).take_ready();
                actor_zeta::detail::ignore_unused(result);
                ++successful;
            }
        }
    }

    // Vacuous on purpose: anywhere from 0 to NUM_MESSAGES may complete before stop();
    // the real check is a clean sanitizer run.
    REQUIRE(successful >= 0);
}

TEST_CASE("Shutdown Test 4.2: Graceful shutdown - wait for all futures") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);
    scheduler->start();

    std::vector<actor_zeta::unique_future<int>> futures;

    {
        auto actor = actor_zeta::spawn<shutdown_test_actor>(resource);

        constexpr int NUM_MESSAGES = 10;
        for (int i = 0; i < NUM_MESSAGES; ++i) {
            auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                          &shutdown_test_actor::slow_task, i);

            if (needs_sched) {
                scheduler->enqueue(actor.get());
            }

            futures.push_back(std::move(future));
        }

        // The scheduler is still running, so every future must be complete
        // before the actor goes out of scope.
        std::vector<int> results;
        results.reserve(futures.size());
        for (auto& f : futures) {
            while (!f.is_ready()) {
                std::this_thread::yield();
            }
            results.push_back(std::move(f).take_ready());
        }
        int completed = 0;
        for (size_t i = 0; i < results.size(); ++i) {
            REQUIRE(results[i] == static_cast<int>(i) * 2);
            ++completed;
        }

        REQUIRE(completed == NUM_MESSAGES);
    }

    scheduler->stop();
}

TEST_CASE("Shutdown Test 4.3: Rapid shutdown with pending work") {
    auto* resource = std::pmr::get_default_resource();

    constexpr int NUM_ITERATIONS = 50;

    for (int iter = 0; iter < NUM_ITERATIONS; ++iter) {
        auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 1000);
        scheduler->start();

        auto actor = actor_zeta::spawn<shutdown_test_actor>(resource);

        constexpr int NUM_MESSAGES = 100;
        std::vector<actor_zeta::unique_future<int>> futures;
        futures.reserve(NUM_MESSAGES);

        for (int i = 0; i < NUM_MESSAGES; ++i) {
            auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                          &shutdown_test_actor::slow_task, i);

            if (needs_sched) {
                scheduler->enqueue(actor.get());
            }

            futures.push_back(std::move(future));
        }

        if (iter % 3 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else if (iter % 3 == 1) {
            std::this_thread::yield();
        }

        // stop() before the actor goes out of scope at the end of the iteration.
        scheduler->stop();
    }

    REQUIRE(true);
}

TEST_CASE("Shutdown Test 4.4: Sequential create-destroy cycles") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);
    scheduler->start();

    constexpr int NUM_CYCLES = 50;
    std::atomic<int> completed_cycles{0};

    for (int i = 0; i < NUM_CYCLES; ++i) {
        auto actor = actor_zeta::spawn<shutdown_test_actor>(resource);

        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                      &shutdown_test_actor::slow_task, i);

        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        switch (i % 4) {
            case 0:
                break;
            case 1:
                std::this_thread::yield();
                break;
            case 2:
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                break;
            case 3:
                break;
        }

        // The destructor asserts the actor is not running, so wait before it goes
        // out of scope.
        while (!future.is_ready()) {
            std::this_thread::yield();
        }
        int result = std::move(future).take_ready();
        REQUIRE(result == i * 2);

        completed_cycles.fetch_add(1, std::memory_order_relaxed);
    }

    scheduler->stop();
    REQUIRE(completed_cycles.load() == NUM_CYCLES);
}
