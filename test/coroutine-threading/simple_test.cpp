#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/dispatch.hpp>

#include <vector>

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


/// @brief Client актор - использует только address_t для связи с worker
/// Resume управляется supervisor'ом снаружи
class client_actor final : public basic_actor<client_actor> {
public:
    explicit client_actor(pmr::memory_resource* resource, address_t worker_address)
        : basic_actor<client_actor>(resource)
        , worker_address_(worker_address)
        , final_result_(0) {
    }

    /// @brief Корутина - отправляет запрос и ждёт результат
    /// Suspend/resume управляется supervisor'ом
    unique_future<int> process(int x) {
        std::cerr << "[client::process] START x=" << x << std::endl;

        // Отправляем запрос worker'у через address_t (только адрес!)
        auto future = send(worker_address_, address(), &worker_actor::compute, x);

        // co_await - suspend если не готово
        // Supervisor возобновит корутину когда future станет ready
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
            case msg_id<client_actor, &client_actor::process>: {
                // CRITICAL: Must store pending coroutine future!
                // If we just return dispatch() result, it gets destroyed immediately,
                // which destroys the coroutine and causes refcount underflow.
                auto future = dispatch(this, &client_actor::process, msg);
                if (!future.available()) {
                    pending_.push_back(std::move(future));
                }
                return make_ready_future_void(resource());
            }
            case msg_id<client_actor, &client_actor::get_result>:
                return dispatch(this, &client_actor::get_result, msg);
            default:
                break;
        }
        return make_ready_future_void(resource());
    }

    /// @brief Poll pending coroutines and resume if ready
    /// @return true if there are still pending coroutines
    bool poll_pending() {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->awaiting_ready()) {
                it->resume();
                if (it->available()) {
                    it = pending_.erase(it);
                    continue;
                }
            }
            ++it;
        }
        return !pending_.empty();
    }

    /// @brief Check if there are pending coroutines
    bool has_pending() const { return !pending_.empty(); }

    ~client_actor() = default;

private:
    address_t worker_address_;
    std::atomic<int> final_result_;
    std::vector<unique_future<int>> pending_;  // Store pending coroutine futures
};


/// @brief Простой supervisor - управляет resume() акторов
/// Это отдельная сущность которая знает о всех акторах
/// Для теста: принимает конкретные типы акторов
class simple_supervisor {
public:
    void set_actors(worker_actor* w, client_actor* c = nullptr) {
        worker_ = w;
        client_ = c;
    }

    /// @brief Запускает акторов пока future не станет ready
    template<typename T>
    void run_until_ready(unique_future<T>& future, int max_iterations = 1000) {
        int iterations = 0;
        while (!future.available() && iterations < max_iterations) {
            run_once();
            ++iterations;
        }
    }

    /// @brief Запускает один цикл resume для всех акторов
    void run_once() {
        if (client_) client_->resume(1);
        if (worker_) worker_->resume(1);
        // Poll pending coroutines after processing messages
        if (client_) client_->poll_pending();
    }

private:
    worker_actor* worker_ = nullptr;
    client_actor* client_ = nullptr;
};


TEST_CASE("worker only") {
    auto* resource = pmr::get_default_resource();
    auto worker = spawn<worker_actor>(resource);

    // Supervisor управляет resume
    simple_supervisor supervisor;
    supervisor.set_actors(worker.get());

    auto future = send(worker.get(), address_t::empty_address(), &worker_actor::compute, 21);

    // Supervisor запускает акторов
    supervisor.run_until_ready(future);

    REQUIRE(future.available());
    REQUIRE(std::move(future).get() == 42);
}


TEST_CASE("client-worker coroutine with supervisor") {
    auto* resource = pmr::get_default_resource();
    auto worker = spawn<worker_actor>(resource);
    auto client = spawn<client_actor>(resource, worker->address());

    // Supervisor знает о всех акторах и управляет их resume
    simple_supervisor supervisor;
    supervisor.set_actors(worker.get(), client.get());

    // Отправляем сообщение client'у
    auto future = send(client.get(), address_t::empty_address(), &client_actor::process, 21);

    // Supervisor управляет resume всех акторов
    supervisor.run_until_ready(future);

    REQUIRE(future.available());
    REQUIRE(std::move(future).get() == 52);  // 21 * 2 + 10 = 52

    // Проверяем через get_result()
    auto result_future = send(client.get(), address_t::empty_address(), &client_actor::get_result);
    supervisor.run_until_ready(result_future);

    REQUIRE(result_future.available());
    REQUIRE(std::move(result_future).get() == 52);
}