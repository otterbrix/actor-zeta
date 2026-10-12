#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/scheduler.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

class worker_actor final : public actor_zeta::basic_actor<worker_actor> {
public:
    explicit worker_actor(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<worker_actor>(ptr) {
    }

    actor_zeta::unique_future<void> ping() {
        handled_.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }

    std::size_t handled() const noexcept { return handled_.load(std::memory_order_acquire); }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        if (msg->command() == actor_zeta::msg_id<worker_actor, &worker_actor::ping>) {
            co_await dispatch(this, &worker_actor::ping, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&worker_actor::ping>;

private:
    std::atomic<std::size_t> handled_{0};
};

class balancer_actor final : public actor_zeta::actor::actor_mixin<balancer_actor> {
public:
    template<typename T> using unique_future = actor_zeta::unique_future<T>;

    explicit balancer_actor(std::pmr::memory_resource* resource)
        : actor_zeta::actor::actor_mixin<balancer_actor>()
        , resource_(resource) {
    }

    ~balancer_actor() {
        workers_.clear();
    }

    std::pmr::memory_resource* resource() const noexcept { return resource_; }

    void add_worker() {
        auto worker = actor_zeta::spawn<worker_actor>(resource_);
        workers_.emplace_back(std::move(worker));
    }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* /*msg*/) {
        co_return;
    }

private:
    std::pmr::memory_resource* resource_;
    std::vector<worker_actor::unique_actor> workers_;
};

TEST_CASE("shutdown - basic test") {
    auto* resource =std::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(resource, 1, 100));

    auto actor = actor_zeta::spawn<worker_actor>(resource);

    std::vector<actor_zeta::unique_future<void>> futures;
    for (int i = 0; i < 3; ++i) {
        auto [needs_sched, future] = actor_zeta::send(actor.get(), &worker_actor::ping);
        if (needs_sched) { scheduler->enqueue(actor.get()); }
        futures.push_back(std::move(future));
    }

    scheduler->start();
    // Wait for progress, not a fixed nap: a dropped needs_sched must reach the
    // assertion below instead of being hidden by the sleep.
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (actor->handled() < futures.size()
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
    }
    scheduler->stop();

    const std::size_t handled = actor->handled();
    actor.reset();   // cancels whatever never ran

    // Undisturbed shutdown: every message runs. handled + failed == size would
    // also hold with nothing handled at all, so require the stronger form here.
    std::size_t settled = 0;
    for (auto& f : futures) {
        if (f.is_ready()) { ++settled; }
    }
    REQUIRE(settled == futures.size());
    REQUIRE(handled == futures.size());
}

TEST_CASE("shutdown - multiple actors") {
    auto* resource =std::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(resource, 1, 100));

    std::vector<std::unique_ptr<worker_actor, actor_zeta::pmr::deleter_t>> actors;
    for (int i = 0; i < 3; ++i) {
        actors.push_back(actor_zeta::spawn<worker_actor>(resource));
    }

    std::vector<actor_zeta::unique_future<void>> futures;
    for (auto& actor : actors) {
        for (int i = 0; i < 2; ++i) {
            auto [needs_sched, future] = actor_zeta::send(actor.get(), &worker_actor::ping);
            if (needs_sched) { scheduler->enqueue(actor.get()); }
            futures.push_back(std::move(future));
        }
    }

    scheduler->start();
    auto total_handled = [&actors]() {
        std::size_t n = 0;
        for (auto& a : actors) { n += a->handled(); }
        return n;
    };
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (total_handled() < futures.size()
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
    }
    scheduler->stop();

    const std::size_t handled = total_handled();
    actors.clear();   // cancels whatever never ran

    std::size_t settled = 0;
    for (auto& f : futures) {
        if (f.is_ready()) { ++settled; }
    }
    REQUIRE(settled == futures.size());
    REQUIRE(handled == futures.size());
}

TEST_CASE("shutdown - immediate stop") {
    auto* resource =std::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(resource, 1, 100));

    auto actor = actor_zeta::spawn<worker_actor>(resource);

    std::vector<actor_zeta::unique_future<void>> futures;
    for (int i = 0; i < 10; ++i) {
        auto [needs_sched, future] = actor_zeta::send(actor.get(), &worker_actor::ping);
        if (needs_sched) { scheduler->enqueue(actor.get()); }
        futures.push_back(std::move(future));
    }

    scheduler->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    scheduler->stop();

    const std::size_t handled = actor->handled();
    actor.reset();   // cancels whatever never ran

    // Nothing is lost: every message either ran or was cancelled at teardown.
    std::size_t settled = 0;
    std::size_t failed = 0;
    for (auto& f : futures) {
        if (f.is_ready()) { ++settled; }
        if (f.failed()) { ++failed; }
    }
    REQUIRE(settled == futures.size());
    REQUIRE(handled + failed == futures.size());
}

TEST_CASE("shutdown - concurrent enqueue during destruction") {
    auto* resource =std::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(resource, 1, 100));

    auto actor = actor_zeta::spawn<worker_actor>(resource);
    scheduler->start();

    std::atomic<bool> stop{false};
    std::atomic<int> enqueue_count{0};
    worker_actor* actor_ptr = actor.get();

    std::thread t1([&, actor_ptr] {
        while (!stop.load(std::memory_order_relaxed)) {
            auto [needs_sched, future] = actor_zeta::send(actor_ptr, &worker_actor::ping);
            if (needs_sched) { scheduler->enqueue(actor_ptr); }
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

    // The real check here is the sanitizer: this races send against teardown.
    // The counter only proves the sender loop actually spun.
    REQUIRE(enqueue_count.load() > 10);
}

TEST_CASE("shutdown - concurrent resume during destruction") {
    auto* resource =std::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(resource, 1, 100));

    auto actor = actor_zeta::spawn<worker_actor>(resource);

    std::vector<actor_zeta::unique_future<void>> futures;
    for (int i = 0; i < 100; ++i) {
        auto [needs_sched, future] = actor_zeta::send(actor.get(), &worker_actor::ping);
        if (needs_sched) { scheduler->enqueue(actor.get()); }
        futures.push_back(std::move(future));
    }

    scheduler->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    scheduler->stop();
    const std::size_t handled = actor->handled();
    actor.reset();

    // Nothing is lost: every message either ran or was cancelled at teardown.
    std::size_t settled = 0;
    std::size_t failed = 0;
    for (auto& f : futures) {
        if (f.is_ready()) { ++settled; }
        if (f.failed()) { ++failed; }
    }
    REQUIRE(settled == futures.size());
    REQUIRE(handled + failed == futures.size());
}

TEST_CASE("shutdown - three-way race: enqueue + resume + destroy") {
    auto* resource =std::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(resource, 2, 100));

    auto actor = actor_zeta::spawn<worker_actor>(resource);

    std::atomic<bool> stop{false};
    std::atomic<int> enqueue_count{0};
    worker_actor* actor_ptr = actor.get();

    std::thread t1([&, actor_ptr] {
        while (!stop.load(std::memory_order_relaxed)) {
            auto [needs_sched, future] = actor_zeta::send(actor_ptr, &worker_actor::ping);
            if (needs_sched) { scheduler->enqueue(actor_ptr); }
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

    // The real check here is the sanitizer: this races send against teardown.
    // The counter only proves the sender loop actually spun.
    REQUIRE(enqueue_count.load() > 10);
}