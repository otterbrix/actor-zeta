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

    void ping() {
        // Empty handler
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

    template<typename R>
    unique_future<R> enqueue_impl(actor_zeta::mailbox::message_ptr msg) {
        return enqueue_sync_impl<R>(std::move(msg), [this](auto* msg) { behavior(msg); });
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