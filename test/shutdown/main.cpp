#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/scheduler.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <chrono>
#include <thread>
#include <vector>

class worker_actor final : public actor_zeta::basic_actor<worker_actor> {
public:
    explicit worker_actor(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<worker_actor>(ptr) {
    }

    actor_zeta::unique_future<void> ping() {
        co_return;
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        if (msg->command() == actor_zeta::msg_id<worker_actor, &worker_actor::ping>) {
            dispatch(this, &worker_actor::ping, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&worker_actor::ping>;
};

class balancer_actor final : public actor_zeta::actor::actor_mixin<balancer_actor> {
public:
    template<typename T> using unique_future = actor_zeta::unique_future<T>;

    balancer_actor(std::pmr::memory_resource* resource, actor_zeta::scheduler::sharing_scheduler* scheduler)
        : actor_zeta::actor::actor_mixin<balancer_actor>()
        , resource_(resource)
        , scheduler_(scheduler) {
    }

    ~balancer_actor() {
        workers_.clear();
    }

    std::pmr::memory_resource* resource() const noexcept { return resource_; }

    void add_worker() {
        auto worker = actor_zeta::spawn<worker_actor>(resource_);
        workers_.emplace_back(std::move(worker));
    }

    void behavior(actor_zeta::mailbox::message* /*msg*/) {
    }

    template<typename ReturnType, typename... Args>
    ReturnType enqueue_impl(
        actor_zeta::actor::address_t sender,
        actor_zeta::mailbox::message_id cmd,
        Args&&... args
    ) {
        using R = typename actor_zeta::type_traits::is_unique_future<ReturnType>::value_type;
        return enqueue_sync_impl<R>(
            sender,
            cmd,
            [this](auto* msg) { behavior(msg); },
            std::forward<Args>(args)...
        );
    }

protected:

private:
    std::pmr::memory_resource* resource_;
    actor_zeta::scheduler::sharing_scheduler* scheduler_;
    size_t cursor_ = 0;
    std::vector<worker_actor::unique_actor> workers_;
};

TEST_CASE("shutdown - basic test") {
    auto* resource =std::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(1, 100));

    auto actor = actor_zeta::spawn<worker_actor>(resource);

    std::vector<decltype(actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), &worker_actor::ping))> futures;
    for (int i = 0; i < 3; ++i) {
        futures.push_back(actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), &worker_actor::ping));
    }

    scheduler->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    scheduler->stop();

    REQUIRE(true);
}

TEST_CASE("shutdown - multiple actors") {
    auto* resource =std::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(1, 100));

    std::vector<std::unique_ptr<worker_actor, actor_zeta::pmr::deleter_t>> actors;
    for (int i = 0; i < 3; ++i) {
        actors.push_back(actor_zeta::spawn<worker_actor>(resource));
    }

    std::vector<decltype(actor_zeta::send(actors[0].get(), actor_zeta::address_t::empty_address(), &worker_actor::ping))> futures;
    for (auto& actor : actors) {
        for (int i = 0; i < 2; ++i) {
            futures.push_back(actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), &worker_actor::ping));
        }
    }

    scheduler->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    scheduler->stop();

    REQUIRE(true);
}

TEST_CASE("shutdown - immediate stop") {
    auto* resource =std::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(1, 100));

    auto actor = actor_zeta::spawn<worker_actor>(resource);

    std::vector<decltype(actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), &worker_actor::ping))> futures;
    for (int i = 0; i < 10; ++i) {
        futures.push_back(actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), &worker_actor::ping));
    }

    scheduler->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    scheduler->stop();

    REQUIRE(true);
}

TEST_CASE("shutdown - concurrent enqueue during destruction") {
    auto* resource =std::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(1, 100));

    auto actor = actor_zeta::spawn<worker_actor>(resource);
    scheduler->start();

    std::atomic<bool> stop{false};
    std::atomic<int> enqueue_count{0};
    worker_actor* actor_ptr = actor.get();

    std::thread t1([&, actor_ptr] {
        while (!stop.load(std::memory_order_relaxed)) {
            auto future = actor_zeta::send(actor_ptr, actor_zeta::address_t::empty_address(), &worker_actor::ping);
            ++enqueue_count;
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    });

    std::thread t2([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        scheduler->stop();
        stop = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        actor.reset();
    });

    t1.join();
    t2.join();

    REQUIRE(enqueue_count.load() > 0);
}

TEST_CASE("shutdown - concurrent resume during destruction") {
    auto* resource =std::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(1, 100));

    auto actor = actor_zeta::spawn<worker_actor>(resource);

    std::vector<decltype(actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), &worker_actor::ping))> futures;
    for (int i = 0; i < 100; ++i) {
        futures.push_back(actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(), &worker_actor::ping));
    }

    scheduler->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    scheduler->stop();
    actor.reset();

    REQUIRE(true);
}

TEST_CASE("shutdown - three-way race: enqueue + resume + destroy") {
    auto* resource =std::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(2, 100));

    auto actor = actor_zeta::spawn<worker_actor>(resource);

    std::atomic<bool> stop{false};
    std::atomic<int> enqueue_count{0};
    worker_actor* actor_ptr = actor.get();

    std::thread t1([&, actor_ptr] {
        while (!stop.load(std::memory_order_relaxed)) {
            auto future = actor_zeta::send(actor_ptr, actor_zeta::address_t::empty_address(), &worker_actor::ping);
            ++enqueue_count;
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    });

    scheduler->start();

    std::thread t3([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        stop = true;
        scheduler->stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        actor.reset();
    });

    t1.join();
    t3.join();

    REQUIRE(enqueue_count.load() > 0);
}