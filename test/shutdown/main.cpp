#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/scheduler.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <thread>
#include <chrono>
#include <vector>

enum class command : uint64_t {
    ping = 0
};

// Basic actor that just handles messages
class worker_actor final : public actor_zeta::basic_actor<worker_actor> {
public:
    explicit worker_actor(actor_zeta::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<worker_actor>(ptr)
        , ping_(actor_zeta::make_behavior(resource(), []() -> void {
            // Empty handler
        })) {
    }

    void behavior(actor_zeta::message* msg) {
        if (msg->command() == actor_zeta::make_message_id(command::ping)) {
            ping_(msg);
        }
    }

private:
    actor_zeta::behavior_t ping_;
};

// Balancer actor that manually enqueues and schedules - THIS reproduces the bug
class balancer_actor final : public actor_zeta::actor_abstract_t {
public:
    balancer_actor(actor_zeta::pmr::memory_resource* resource, actor_zeta::scheduler_t* scheduler)
        : actor_zeta::actor_abstract_t(resource)
        , scheduler_(scheduler) {
    }

    void add_worker() {
        auto worker = actor_zeta::spawn<worker_actor>(resource());
        workers_.emplace_back(std::move(worker));
    }

protected:
    bool enqueue_impl(actor_zeta::message_ptr msg) override {
        if (workers_.empty()) {
            return false;
        }

        auto index = cursor_ % workers_.size();
        workers_[index]->enqueue(std::move(msg));
        scheduler_->schedule(workers_[index].get());  // Manual schedule - race condition here!
        ++cursor_;
        return true;
    }

private:
    actor_zeta::scheduler_t* scheduler_;
    size_t cursor_ = 0;
    std::vector<worker_actor::unique_actor> workers_;
};

TEST_CASE("shutdown - basic test") {
    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = actor_zeta::scheduler::make_sharing_scheduler(resource, 1, 100);

    auto actor = actor_zeta::spawn<worker_actor>(resource);

    // Send some messages
    for (int i = 0; i < 3; ++i) {
        actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), command::ping);
    }

    scheduler->start();

    // Wait for processing
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // This should not crash
    scheduler->stop();

    REQUIRE(true); // If we get here, no crash occurred
}

TEST_CASE("shutdown - multiple actors") {
    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = actor_zeta::scheduler::make_sharing_scheduler(resource, 1, 100);

    // Create multiple actors
    std::vector<std::unique_ptr<worker_actor, actor_zeta::pmr::deleter_t>> actors;
    for (int i = 0; i < 3; ++i) {
        actors.push_back(actor_zeta::spawn<worker_actor>(resource));
    }

    // Send messages to all actors
    for (auto& actor : actors) {
        for (int i = 0; i < 2; ++i) {
            actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), command::ping);
        }
    }

    scheduler->start();

    // Wait for processing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // This should not crash
    scheduler->stop();

    REQUIRE(true); // If we get here, no crash occurred
}

TEST_CASE("shutdown - immediate stop") {
    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = actor_zeta::scheduler::make_sharing_scheduler(resource, 1, 100);

    auto actor = actor_zeta::spawn<worker_actor>(resource);

    // Send messages
    for (int i = 0; i < 10; ++i) {
        actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), command::ping);
    }

    scheduler->start();

    // Stop immediately without waiting
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    scheduler->stop();

    REQUIRE(true); // If we get here, no crash occurred
}

TEST_CASE("shutdown - balancer with manual schedule (reproduces bug)") {
    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = actor_zeta::scheduler::make_sharing_scheduler(resource, 1, 100);

    auto balancer = actor_zeta::spawn<balancer_actor>(resource, scheduler.get());

    // Add 3 workers
    balancer->add_worker();
    balancer->add_worker();
    balancer->add_worker();

    // Send messages through balancer
    for (int i = 0; i < 6; ++i) {
        actor_zeta::send(balancer.get(), actor_zeta::address_t::empty_address(), command::ping);
    }

    scheduler->start();

    // Wait for processing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // This SHOULD crash with assertion failure in lifo_inbox.hpp:64
    scheduler->stop();

    REQUIRE(true); // If we get here, no crash occurred
}