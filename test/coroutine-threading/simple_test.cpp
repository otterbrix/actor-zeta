#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>

#include <vector>

using namespace actor_zeta;

class worker_actor final : public basic_actor<worker_actor> {
public:
    explicit worker_actor(std::pmr::memory_resource* resource)
        : basic_actor<worker_actor>(resource) {
    }

    unique_future<int> compute(int x) {
        std::cerr << "[worker::compute] x=" << x << ", returning " << (x * 2) << std::endl;
        co_return x * 2;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&worker_actor::compute>;

    behavior_t behavior(mailbox::message* msg) {
        switch (msg->command()) {
            case msg_id<worker_actor, &worker_actor::compute>:
                co_await dispatch(this, &worker_actor::compute, msg);
                break;
            default:
                break;
        }
    }

    ~worker_actor() = default;
};


class client_actor final : public basic_actor<client_actor> {
public:
    explicit client_actor(std::pmr::memory_resource* resource, address_t worker_address)
        : basic_actor<client_actor>(resource)
        , worker_address_(worker_address)
        , final_result_(0) {
    }

    unique_future<int> process(int x) {
        std::cerr << "[client::process] START x=" << x << std::endl;

        auto [needs_sched, future] = send(worker_address_, &worker_actor::compute, x);

        int result = co_await std::move(future);
        std::cerr << "[client::process] Got result: " << result << std::endl;

        final_result_ = result + 10;
        co_return final_result_.load();
    }

    unique_future<int> get_result() {
        co_return final_result_.load();
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &client_actor::process,
        &client_actor::get_result
    >;

    behavior_t behavior(mailbox::message* msg) {
        switch (msg->command()) {
            case msg_id<client_actor, &client_actor::process>:
                co_await dispatch(this, &client_actor::process, msg);
                break;
            case msg_id<client_actor, &client_actor::get_result>:
                co_await dispatch(this, &client_actor::get_result, msg);
                break;
            default:
                break;
        }
    }

    bool poll_pending() {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->is_ready()) {
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        return !pending_.empty();
    }

    bool has_pending() const { return !pending_.empty(); }

    ~client_actor() = default;

private:
    address_t worker_address_;
    std::atomic<int> final_result_;
    std::vector<unique_future<void>> pending_;
};


/// Drives resume() on both actors from the test thread; there is no scheduler.
class simple_supervisor {
public:
    void set_actors(worker_actor* w, client_actor* c = nullptr) {
        worker_ = w;
        client_ = c;
    }

    /// Returns true while at least one actor still asks to be rescheduled; the
    /// caller's loop discharges that verdict by coming round again.
    bool run_once() {
        bool wants_more = false;
        if (client_) {
            wants_more |= client_->resume(1).result == actor_zeta::scheduler::resume_result::resume;
        }
        if (worker_) {
            wants_more |= worker_->resume(1).result == actor_zeta::scheduler::resume_result::resume;
        }
        return wants_more;
    }

private:
    worker_actor* worker_ = nullptr;
    client_actor* client_ = nullptr;
};


TEST_CASE("worker only") {
    auto* resource = std::pmr::get_default_resource();
    auto worker = spawn<worker_actor>(resource);

    simple_supervisor supervisor;
    supervisor.set_actors(worker.get());

    auto [needs_sched, future] = send(worker.get(), &worker_actor::compute, 21);

    // Bounded so a mis-wired pump fails the assertion below instead of hanging.
    constexpr int kPumpCap = 64;
    for (int i = 0; i < kPumpCap && !future.is_ready(); ++i) {
        supervisor.run_once();
    }
    REQUIRE(future.is_ready());
    int result = std::move(future).take_ready();

    REQUIRE(result == 42);
}


TEST_CASE("client-worker coroutine with supervisor") {
    auto* resource = std::pmr::get_default_resource();
    auto worker = spawn<worker_actor>(resource);
    auto client = spawn<client_actor>(resource, worker->address());

    simple_supervisor supervisor;
    supervisor.set_actors(worker.get(), client.get());

    auto [needs_sched, future] = send(client.get(), &client_actor::process, 21);

    // Bounded so a mis-wired pump fails the assertion below instead of hanging.
    constexpr int kPumpCap = 64;
    for (int i = 0; i < kPumpCap && !future.is_ready(); ++i) {
        supervisor.run_once();
    }
    REQUIRE(future.is_ready());
    int result = std::move(future).take_ready();

    REQUIRE(result == 52);  // 21 * 2 + 10 = 52

    auto [needs_sched2, result_future] = send(client.get(), &client_actor::get_result);
    for (int i = 0; i < kPumpCap && !result_future.is_ready(); ++i) {
        supervisor.run_once();
    }
    REQUIRE(result_future.is_ready());
    int verified = std::move(result_future).take_ready();

    REQUIRE(verified == 52);
}