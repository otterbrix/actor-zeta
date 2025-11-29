#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/dispatch.hpp>

using namespace actor_zeta;

/// @brief Worker актор - обрабатывает запросы
class worker_actor final : public basic_actor<worker_actor> {
public:
    explicit worker_actor(pmr::memory_resource* resource)
        : basic_actor<worker_actor>(resource) {
    }

    unique_future<int> compute(int x) {
        std::cerr << "[worker::compute] x=" << x << ", returning " << (x * 2) << std::endl;
        return make_ready_future(resource(), x * 2);
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&worker_actor::compute>;

    unique_future<void> behavior(mailbox::message* msg) {
        switch (msg->command()) {
            case msg_id<worker_actor, &worker_actor::compute>:
                return dispatch(this, &worker_actor::compute, msg);
            default:
                break;
        }
        return make_ready_future_void(resource());
    }

    ~worker_actor() = default;
};


/// @brief Client актор - как balancer, но на корутинах
class client_actor final : public basic_actor<client_actor> {
public:
    explicit client_actor(pmr::memory_resource* resource, worker_actor* worker)
        : basic_actor<client_actor>(resource)
        , worker_(worker)
        , final_result_(0) {
    }

    /// @brief Корутина с inline execution (как balancer)
    unique_future<int> process(int x) {
        std::cerr << "[client::process] START x=" << x << std::endl;

        // Отправляем запрос worker'у
        auto future = send(worker_, address(), &worker_actor::compute, x);

        // Inline execution - как в balancer!
        while (!future.is_ready()) {
            std::cerr << "[client::process] Worker not ready, resuming..." << std::endl;
            worker_->resume(1);
        }

        // co_await на ГОТОВУЮ future - НЕ suspend!
        int result = co_await std::move(future);
        std::cerr << "[client::process] Got result: " << result << std::endl;

        final_result_ = result + 10;
        co_return final_result_.load();
    }

    unique_future<int> get_result() {
        return make_ready_future(resource(), final_result_.load());
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &client_actor::process,
        &client_actor::get_result
    >;

    unique_future<void> behavior(mailbox::message* msg) {
        switch (msg->command()) {
            case msg_id<client_actor, &client_actor::process>:
                return dispatch(this, &client_actor::process, msg);
            case msg_id<client_actor, &client_actor::get_result>:
                return dispatch(this, &client_actor::get_result, msg);
            default:
                break;
        }
        return make_ready_future_void(resource());
    }

    ~client_actor() = default;

private:
    worker_actor* worker_;
    std::atomic<int> final_result_;
};


TEST_CASE("worker only") {
    auto* resource = pmr::get_default_resource();
    auto worker = spawn<worker_actor>(resource);

    auto future = send(worker.get(), address_t::empty_address(), &worker_actor::compute, 21);
    worker->resume(1);

    REQUIRE(future.is_ready());
    REQUIRE(std::move(future).get() == 42);
}


TEST_CASE("client-worker coroutine (like balancer)") {
    auto* resource = pmr::get_default_resource();
    auto worker = spawn<worker_actor>(resource);
    auto client = spawn<client_actor>(resource, worker.get());

    // Отправляем сообщение client'у
    auto future = send(client.get(), address_t::empty_address(), &client_actor::process, 21);

    // Resume client - он внутри вызовет worker->resume() (inline execution)
    client->resume(1);

    // Результат готов сразу
    REQUIRE(future.is_ready());
    REQUIRE(std::move(future).get() == 52);  // 21 * 2 + 10 = 52

    // Проверяем через get_result()
    auto result_future = send(client.get(), address_t::empty_address(), &client_actor::get_result);
    client->resume(1);

    REQUIRE(result_future.is_ready());
    REQUIRE(std::move(result_future).get() == 52);
}