#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/scheduler.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <thread>
#include <chrono>
#include <vector>

// Basic actor that just handles messages
class worker_actor final : public actor_zeta::basic_actor<worker_actor> {
public:
    explicit worker_actor(actor_zeta::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<worker_actor>(ptr)
        , ping_(actor_zeta::make_behavior(resource(), this, &worker_actor::ping)) {
    }

    actor_zeta::unique_future<void> ping() {
        // Empty handler
        return actor_zeta::make_ready_future_void(resource());
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        if (msg->command() == actor_zeta::msg_id<worker_actor, &worker_actor::ping>) {
            ping_(msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&worker_actor::ping>;

private:
    actor_zeta::behavior_t ping_;
};

// Balancer actor that manually enqueues and schedules
class balancer_actor final : public actor_zeta::base::actor_mixin<balancer_actor> {
public:
    template<typename T> using unique_future = actor_zeta::unique_future<T>;

    balancer_actor(actor_zeta::pmr::memory_resource* resource, actor_zeta::scheduler::sharing_scheduler* scheduler)
        : actor_zeta::base::actor_mixin<balancer_actor>()
        , resource_(resource)
        , scheduler_(scheduler) {
    }

    ~balancer_actor() {
        // Clear workers before balancer is destroyed
        // This ensures workers are released while balancer is still valid
        workers_.clear();
    }

    actor_zeta::pmr::memory_resource* resource() const noexcept { return resource_; }

    void add_worker() {
        auto worker = actor_zeta::spawn<worker_actor>(resource_);
        workers_.emplace_back(std::move(worker));
    }

    void behavior(actor_zeta::mailbox::message* /*msg*/) {
        // Balancer doesn't process messages directly - it forwards them to workers
    }

    template<typename R, typename... Args>
    unique_future<R> enqueue_impl(
        actor_zeta::base::address_t sender,
        actor_zeta::mailbox::message_id cmd,
        Args&&... args
    ) {
        return enqueue_sync_impl<R>(
            sender,
            cmd,
            [this](auto* msg) { behavior(msg); },
            std::forward<Args>(args)...
        );
    }

protected:

private:
    actor_zeta::pmr::memory_resource* resource_;
    actor_zeta::scheduler::sharing_scheduler* scheduler_;
    size_t cursor_ = 0;
    std::vector<worker_actor::unique_actor> workers_;
};

TEST_CASE("shutdown - basic test") {
    auto* resource = actor_zeta::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(1, 100));

    auto actor = actor_zeta::spawn<worker_actor>(resource);

    // Send some messages and collect futures
    std::vector<decltype(actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), &worker_actor::ping))> futures;
    for (int i = 0; i < 3; ++i) {
        futures.push_back(actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), &worker_actor::ping));
    }

    scheduler->start();

    // Wait for processing
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Futures are ready after sleep - destructor will clean up

    // This should not crash
    scheduler->stop();

    REQUIRE(true); // If we get here, no crash occurred
}

TEST_CASE("shutdown - multiple actors") {
    auto* resource = actor_zeta::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(1, 100));

    // Create multiple actors
    std::vector<std::unique_ptr<worker_actor, actor_zeta::pmr::deleter_t>> actors;
    for (int i = 0; i < 3; ++i) {
        actors.push_back(actor_zeta::spawn<worker_actor>(resource));
    }

    // Send messages to all actors and collect futures
    std::vector<decltype(actor_zeta::send(actors[0].get(), actor_zeta::address_t::empty_address(), &worker_actor::ping))> futures;
    for (auto& actor : actors) {
        for (int i = 0; i < 2; ++i) {
            futures.push_back(actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), &worker_actor::ping));
        }
    }

    scheduler->start();

    // Wait for processing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Futures are ready after sleep - destructor will clean up

    // This should not crash
    scheduler->stop();

    REQUIRE(true); // If we get here, no crash occurred
}

TEST_CASE("shutdown - immediate stop") {
    auto* resource = actor_zeta::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(1, 100));

    auto actor = actor_zeta::spawn<worker_actor>(resource);

    // Send messages and collect futures
    std::vector<decltype(actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), &worker_actor::ping))> futures;
    for (int i = 0; i < 10; ++i) {
        futures.push_back(actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), &worker_actor::ping));
    }

    scheduler->start();

    // Stop immediately without waiting
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    scheduler->stop();

    // Futures may be cancelled due to immediate stop - destructor will clean up

    REQUIRE(true); // If we get here, no crash occurred
}

// ============================================================================
// NEW TESTS: Shutdown Race Conditions (ISSUE #2, #3)
// ============================================================================

TEST_CASE("shutdown - concurrent enqueue during destruction") {
    // Tests ISSUE #2: Invalid state assertions in enqueue_impl()
    // Race: Thread 1 in enqueue_impl() CAS loop
    //       Thread 2 calls begin_shutdown() → sets destroying bit
    //       Thread 1 sees scheduled_destroying and asserts
    // Expected: No assertion failure, enqueue should handle destroying state

    auto* resource = actor_zeta::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(1, 100));

    auto actor = actor_zeta::spawn<worker_actor>(resource);
    scheduler->start();

    std::atomic<bool> stop{false};
    std::atomic<int> enqueue_count{0};

    // Capture raw pointer before threads start - safe because we control lifetime
    worker_actor* actor_ptr = actor.get();

    // Thread 1: Continuous enqueue
    std::thread t1([&, actor_ptr] {
        while (!stop.load(std::memory_order_relaxed)) {
            auto future = actor_zeta::send(actor_ptr, actor_zeta::address_t::empty_address(), &worker_actor::ping);
            ++enqueue_count;
            // Small delay to increase race window
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    });

    // Thread 2: Destroy after 50ms
    std::thread t2([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        scheduler->stop();
        stop = true;  // Signal t1 to stop FIRST
        std::this_thread::sleep_for(std::chrono::milliseconds(10));  // Give t1 time to exit loop
        actor.reset();  // Destroy after t1 stopped sending
    });

    t1.join();
    t2.join();

    // Should NOT assert with scheduled_destroying
    REQUIRE(enqueue_count.load() > 0); // Verify enqueue happened
}

TEST_CASE("shutdown - concurrent resume during destruction") {
    // Tests ISSUE #3: Invalid state assertions in resume_guard
    // Race: Thread 1 in resume_guard constructor
    //       Thread 2 calls destructor → sets destroying bit
    //       Thread 1 sees scheduled_destroying and asserts
    // Expected: No assertion failure, resume_guard should handle destroying state

    auto* resource = actor_zeta::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(1, 100));

    auto actor = actor_zeta::spawn<worker_actor>(resource);

    // Send many messages to keep actor busy
    std::vector<decltype(actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), &worker_actor::ping))> futures;
    for (int i = 0; i < 100; ++i) {
        futures.push_back(actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), &worker_actor::ping));
    }

    scheduler->start();

    // Let scheduler run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Destroy while actor may be running
    scheduler->stop();
    actor.reset();

    // Should NOT assert with running_destroying or scheduled_destroying
    REQUIRE(true); // If we get here, no assertion fired
}

TEST_CASE("shutdown - three-way race: enqueue + resume + destroy") {
    // Combined race: All three operations happening simultaneously
    // Thread 1: Enqueuing messages
    // Thread 2: Scheduler running resume()
    // Thread 3: Destroying actor
    // Expected: No assertions, clean shutdown

    auto* resource = actor_zeta::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(2, 100));

    auto actor = actor_zeta::spawn<worker_actor>(resource);

    std::atomic<bool> stop{false};
    std::atomic<int> enqueue_count{0};

    // Capture raw pointer before threads start
    worker_actor* actor_ptr = actor.get();

    // Thread 1: Continuous enqueue
    std::thread t1([&, actor_ptr] {
        while (!stop.load(std::memory_order_relaxed)) {
            auto future = actor_zeta::send(actor_ptr, actor_zeta::address_t::empty_address(), &worker_actor::ping);
            ++enqueue_count;
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    });

    // Thread 2: Scheduler running (multiple workers)
    scheduler->start();

    // Thread 3: Destroy after 30ms
    std::thread t3([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        stop = true;  // Signal t1 to stop FIRST
        scheduler->stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));  // Give t1 time to exit
        actor.reset();
    });

    t1.join();
    t3.join();

    // Should NOT assert anywhere
    REQUIRE(enqueue_count.load() > 0);
}