#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <thread>
#include <vector>

// Stress, not proof: an actor destroyed under load must not be resumed or
// enqueued after its members are gone. What protects the teardown is
// ~cooperative_actor's body -- publish `destroying`, then
// wait_for_activity_to_drain() for the thread holding `running` and for any
// sender already past enqueue_impl's gate. Meaningful under TSan and ASan.

class good_shutdown_actor final : public actor_zeta::basic_actor<good_shutdown_actor> {
public:
    explicit good_shutdown_actor(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<good_shutdown_actor>(resource)
        , counter_(0) {
    }

    // ~cooperative_actor does the waiting; nothing to add here.
    ~good_shutdown_actor() = default;

    actor_zeta::unique_future<int> slow_task(int value) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        co_return value * 2;
    }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<good_shutdown_actor, &good_shutdown_actor::slow_task>) {
            co_await dispatch(this, &good_shutdown_actor::slow_task, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &good_shutdown_actor::slow_task
    >;

    size_t processed_count() const { return counter_.load(std::memory_order_acquire); }

private:
    std::atomic<size_t> counter_;
};

TEST_CASE("Aggressive Shutdown Test: Automatic teardown under load") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 1000);
    scheduler->start();

    constexpr int NUM_ITERATIONS = 10;

    for (int iter = 0; iter < NUM_ITERATIONS; ++iter) {
        std::vector<actor_zeta::unique_future<int>> futures;

        {
            auto actor = actor_zeta::spawn<good_shutdown_actor>(resource);

            constexpr int NUM_MESSAGES = 100;
            for (int i = 0; i < NUM_MESSAGES; ++i) {
                auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                              &good_shutdown_actor::slow_task, i);

                if (needs_sched) {
                    scheduler->enqueue(actor.get());
                }

                if (i % 10 == 0) {
                    futures.push_back(std::move(future));
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        futures.clear();
    }

    scheduler->stop();

    // The pass is reaching here without a sanitizer report.
    REQUIRE(true);
}

TEST_CASE("Stress Test: Concurrent actor creation/destruction") {
    auto* resource =std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(8, 1000);
    scheduler->start();

    std::atomic<int> completed{0};
    constexpr int NUM_THREADS = 4;
    constexpr int NUM_ACTORS = 48;  // Must be divisible by NUM_THREADS

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < NUM_ACTORS / NUM_THREADS; ++i) {
                {
                    auto actor = actor_zeta::spawn<good_shutdown_actor>(resource);

                    for (int j = 0; j < 10; ++j) {
                        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                                      &good_shutdown_actor::slow_task, j);
                        if (needs_sched) {
                            scheduler->enqueue(actor.get());
                        }
                    }

                    // So that some messages are still queued at destruction.
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }

                completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    scheduler->stop();

    REQUIRE(completed.load() == NUM_ACTORS);
}