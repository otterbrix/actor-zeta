#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>

#include <vector>

using namespace actor_zeta;

/// @brief Worker actor - handles requests
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


/// @brief Client actor - uses only address_t for communication with worker
/// Resume is controlled by supervisor externally
class client_actor final : public basic_actor<client_actor> {
public:
    explicit client_actor(std::pmr::memory_resource* resource, address_t worker_address)
        : basic_actor<client_actor>(resource)
        , worker_address_(worker_address)
        , final_result_(0) {
    }

    /// @brief Coroutine - sends request and waits for result
    /// Suspend/resume is controlled by supervisor
    unique_future<int> process(int x) {
        std::cerr << "[client::process] START x=" << x << std::endl;

        // Send request to worker via address_t (only address!)
        auto [needs_sched, future] = send(worker_address_, &worker_actor::compute, x);

        // co_await - suspend if not ready
        // Supervisor will resume coroutine when future becomes ready
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

    /// @brief Clean up completed pending futures
    /// @return true if there are still pending coroutines
    /// With auto-resume in set_value(), coroutines resume automatically
    bool poll_pending() {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->available()) {
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        return !pending_.empty();
    }

    /// @brief Check if there are pending coroutines
    bool has_pending() const { return !pending_.empty(); }

    ~client_actor() = default;

private:
    address_t worker_address_;
    std::atomic<int> final_result_;
    std::vector<unique_future<void>> pending_;  // Store pending dispatch futures
};


/// @brief Simple supervisor - controls resume() of actors
/// This is a separate entity that knows about all actors
/// For test: accepts concrete actor types
class simple_supervisor {
public:
    void set_actors(worker_actor* w, client_actor* c = nullptr) {
        worker_ = w;
        client_ = c;
    }

    /// @brief Runs actors until future becomes ready
    template<typename T>
    void run_until_ready(unique_future<T>& future, int max_iterations = 1000) {
        int iterations = 0;
        while (!future.available() && iterations < max_iterations) {
            run_once();
            ++iterations;
        }
    }

    /// @brief Runs one resume cycle for all actors
    void run_once() {
        if (client_) client_->resume(1);
        if (worker_) worker_->resume(1);
        // With behavior_t API, coroutines resume automatically when futures ready
    }

private:
    worker_actor* worker_ = nullptr;
    client_actor* client_ = nullptr;
};


TEST_CASE("worker only") {
    auto* resource = std::pmr::get_default_resource();
    auto worker = spawn<worker_actor>(resource);

    // Supervisor controls resume
    simple_supervisor supervisor;
    supervisor.set_actors(worker.get());

    auto [needs_sched, future] = send(worker.get(), &worker_actor::compute, 21);

    // Supervisor runs actors
    supervisor.run_until_ready(future);

    REQUIRE(future.available());
    REQUIRE(std::move(future).get() == 42);
}


TEST_CASE("client-worker coroutine with supervisor") {
    auto* resource = std::pmr::get_default_resource();
    auto worker = spawn<worker_actor>(resource);
    auto client = spawn<client_actor>(resource, worker->address());

    // Supervisor knows about all actors and controls their resume
    simple_supervisor supervisor;
    supervisor.set_actors(worker.get(), client.get());

    // Send message to client
    auto [needs_sched, future] = send(client.get(), &client_actor::process, 21);

    // Supervisor controls resume of all actors
    supervisor.run_until_ready(future);

    REQUIRE(future.available());
    REQUIRE(std::move(future).get() == 52);  // 21 * 2 + 10 = 52

    // Verify via get_result()
    auto [needs_sched2, result_future] = send(client.get(), &client_actor::get_result);
    supervisor.run_until_ready(result_future);

    REQUIRE(result_future.available());
    REQUIRE(std::move(result_future).get() == 52);
}