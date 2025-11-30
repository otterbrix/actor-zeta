#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/dispatch.hpp>

#include <thread>
#include <atomic>
#include <sstream>
#include <vector>
#include <mutex>
#include <condition_variable>

using namespace actor_zeta;

// Helper to get thread id as string
std::string thread_id_str() {
    std::ostringstream oss;
    oss << std::this_thread::get_id();
    return oss.str();
}

// Thread-safe logger
class thread_logger {
public:
    void log(const std::string& msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::cerr << msg << std::endl;
    }

    template<typename... Args>
    void log(const char* fmt, Args&&... args) {
        std::ostringstream oss;
        format_impl(oss, fmt, std::forward<Args>(args)...);
        log(oss.str());
    }

private:
    void format_impl(std::ostringstream& oss, const char* fmt) {
        oss << fmt;
    }

    template<typename T, typename... Args>
    void format_impl(std::ostringstream& oss, const char* fmt, T&& val, Args&&... args) {
        while (*fmt) {
            if (*fmt == '%') {
                oss << std::forward<T>(val);
                format_impl(oss, fmt + 1, std::forward<Args>(args)...);
                return;
            }
            oss << *fmt++;
        }
    }

    std::mutex mutex_;
};

thread_logger g_log;

/// @brief Worker актор - обрабатывает запросы
class worker_actor final : public basic_actor<worker_actor> {
public:
    explicit worker_actor(pmr::memory_resource* resource, const std::string& name)
        : basic_actor<worker_actor>(resource)
        , name_(name)
        , compute_count_(0) {
    }

    unique_future<int> compute(int x) {
        ++compute_count_;
        auto tid = thread_id_str();
        g_log.log("[%::compute] thread=% x=% returning % (call #%)",
                  name_, tid, x, x * 2, compute_count_.load());
        last_compute_thread_ = tid;
        return make_ready_future(resource(), x * 2);
    }

    const std::string& last_compute_thread() const { return last_compute_thread_; }
    int compute_count() const { return compute_count_.load(); }
    const std::string& name() const { return name_; }

    using dispatch_traits = actor_zeta::dispatch_traits<&worker_actor::compute>;

    unique_future<void> behavior(mailbox::message* msg) {
        auto tid = thread_id_str();
        g_log.log("[%::behavior] thread=% command=%", name_, tid, msg->command());
        last_behavior_thread_ = tid;

        switch (msg->command()) {
            case msg_id<worker_actor, &worker_actor::compute>:
                return dispatch(this, &worker_actor::compute, msg);
            default:
                g_log.log("[%::behavior] Unknown command!", name_);
                break;
        }
        return make_ready_future_void(resource());
    }

    const std::string& last_behavior_thread() const { return last_behavior_thread_; }

    ~worker_actor() = default;

private:
    std::string name_;
    std::atomic<int> compute_count_;
    std::string last_compute_thread_;
    std::string last_behavior_thread_;
};


/// @brief Client актор - как balancer, но на корутинах
/// Использует только address_t для связи с worker
class client_actor final : public basic_actor<client_actor> {
public:
    /// Пользовательский формат для pending корутин
    struct pending_info {
        unique_future<int> future;
        intrusive_ptr<detail::future_state_base> result_slot;
    };

    explicit client_actor(pmr::memory_resource* resource, address_t worker_address, const std::string& name)
        : basic_actor<client_actor>(resource)
        , worker_address_(worker_address)
        , name_(name)
        , final_result_(0) {
    }

    /// @brief Пользователь вызывает для poll pending корутин
    /// @return true если есть pending корутины
    bool poll_pending() {
        for (auto it = pending_.begin(); it != pending_.end();) {
            auto* state = it->future.get_state();
            if (!state) {
                it = pending_.erase(it);
                continue;
            }

            auto* awaiting = state->get_awaiting_on();
            if (awaiting && awaiting->is_ready()) {
                g_log.log("[%::poll_pending] awaiting ready, resuming coroutine", name_);
                // Resume корутину
                awaiting->resume_coroutine();

                // Проверяем завершилась ли
                if (it->future.is_ready()) {
                    g_log.log("[%::poll_pending] future ready, copying result to slot", name_);
                    // Копируем результат в result_slot
                    if (it->result_slot) {
                        it->result_slot->set_result_rtt(state->take_result());
                    }
                    it = pending_.erase(it);
                    continue;
                }
            }
            ++it;
        }
        return !pending_.empty();
    }

    bool has_pending() const { return !pending_.empty(); }

    /// @brief Корутина - отправляет запрос и ждёт результат
    /// Resume управляется снаружи (тестом или scheduler)
    unique_future<int> process(int x) {
        auto tid_start = thread_id_str();
        g_log.log("\n[%::process] === START === thread=% x=%", name_, tid_start, x);
        process_start_thread_ = tid_start;

        // Отправляем запрос worker'у через address_t
        g_log.log("[%::process] Sending to worker...", name_);
        auto future = send(worker_address_, address(), &worker_actor::compute, x);
        g_log.log("[%::process] future.is_ready()=%", name_, future.is_ready());

        // co_await - suspend если не готово, продолжит когда готово
        auto tid_before_await = thread_id_str();
        g_log.log("[%::process] Before co_await, thread=%", name_, tid_before_await);

        int result = co_await std::move(future);

        auto tid_after_await = thread_id_str();
        g_log.log("[%::process] After co_await, thread=% result=%", name_, tid_after_await, result);
        process_after_await_thread_ = tid_after_await;

        final_result_ = result + 10;
        g_log.log("[%::process] final_result=%", name_, final_result_.load());

        auto tid_end = thread_id_str();
        g_log.log("[%::process] === END === thread=%", name_, tid_end);
        process_end_thread_ = tid_end;

        co_return final_result_.load();
    }

    unique_future<int> get_result() {
        return make_ready_future(resource(), final_result_.load());
    }

    const std::string& process_start_thread() const { return process_start_thread_; }
    const std::string& process_after_await_thread() const { return process_after_await_thread_; }
    const std::string& process_end_thread() const { return process_end_thread_; }
    const std::string& name() const { return name_; }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &client_actor::process,
        &client_actor::get_result
    >;

    unique_future<void> behavior(mailbox::message* msg) {
        auto tid = thread_id_str();
        g_log.log("[%::behavior] thread=% command=%", name_, tid, msg->command());
        last_behavior_thread_ = tid;

        switch (msg->command()) {
            case msg_id<client_actor, &client_actor::process>: {
                // dispatch возвращает future
                auto future = dispatch(this, &client_actor::process, msg);
                if (!future.is_ready()) {
                    // Корутина suspended - сохраняем для polling
                    g_log.log("[%::behavior] process() suspended, storing pending", name_);
                    pending_.push_back({std::move(future), msg->result_slot()});
                }
                // Возвращаем ready void future (мы обработали сообщение)
                return make_ready_future_void(resource());
            }
            case msg_id<client_actor, &client_actor::get_result>:
                return dispatch(this, &client_actor::get_result, msg);
            default:
                g_log.log("[%::behavior] Unknown command!", name_);
                break;
        }
        return make_ready_future_void(resource());
    }

    const std::string& last_behavior_thread() const { return last_behavior_thread_; }

    ~client_actor() = default;

private:
    address_t worker_address_;
    std::string name_;
    std::atomic<int> final_result_;
    std::string last_behavior_thread_;
    std::string process_start_thread_;
    std::string process_after_await_thread_;
    std::string process_end_thread_;
    std::vector<pending_info> pending_;  // Пользовательское хранилище pending корутин
};


// ============================================================================
// SINGLE-THREADED TESTS (baseline)
// ============================================================================

TEST_CASE("single-thread: worker only") {
    auto* resource = pmr::get_default_resource();
    auto worker = spawn<worker_actor>(resource, "Worker");

    g_log.log("\n========== TEST: single-thread: worker only ==========");
    auto main_thread = thread_id_str();
    g_log.log("[TEST] Main thread: %", main_thread);

    auto future = send(worker.get(), address_t::empty_address(), &worker_actor::compute, 21);
    worker->resume(1);

    REQUIRE(future.is_ready());
    int result = std::move(future).get();

    REQUIRE(result == 42);
    REQUIRE(worker->last_compute_thread() == main_thread);

    g_log.log("========== TEST PASSED ==========");
}


TEST_CASE("single-thread: client-worker") {
    auto* resource = pmr::get_default_resource();
    auto worker = spawn<worker_actor>(resource, "Worker");
    auto client = spawn<client_actor>(resource, worker->address(), "Client");

    g_log.log("\n========== TEST: single-thread: client-worker ==========");
    auto main_thread = thread_id_str();
    g_log.log("[TEST] Main thread: %", main_thread);

    auto future = send(client.get(), address_t::empty_address(), &client_actor::process, 21);

    // Тест управляет resume снаружи (ПОЛЬЗОВАТЕЛЬСКИЙ КОД):
    // 1. Resume client - process() отправит сообщение worker'у, suspend на co_await
    //    behavior() сохраняет pending корутину
    client->resume(1);
    g_log.log("[TEST] After client resume, has_pending=%", client->has_pending());
    REQUIRE(client->has_pending());  // Корутина должна быть pending

    // 2. Resume worker - обработает compute(), inner_future станет ready
    worker->resume(1);
    g_log.log("[TEST] After worker resume");

    // 3. Poll pending - проверяет awaiting ready, resume'ит корутину, копирует результат
    client->poll_pending();
    g_log.log("[TEST] After poll_pending, has_pending=%, future.is_ready()=%",
              client->has_pending(), future.is_ready());

    REQUIRE(!client->has_pending());  // Корутина должна завершиться
    REQUIRE(future.is_ready());
    int result = std::move(future).get();

    // 21 * 2 + 10 = 52
    REQUIRE(result == 52);

    // All in main thread
    REQUIRE(client->process_start_thread() == main_thread);
    REQUIRE(client->process_after_await_thread() == main_thread);
    REQUIRE(client->process_end_thread() == main_thread);
    REQUIRE(worker->last_compute_thread() == main_thread);

    g_log.log("========== TEST PASSED ==========");
}


// ============================================================================
// MULTI-THREADED TESTS
// ============================================================================

TEST_CASE("multi-thread: client resumes worker in same thread") {
    auto* resource = pmr::get_default_resource();
    auto worker = spawn<worker_actor>(resource, "Worker");
    auto client = spawn<client_actor>(resource, worker->address(), "Client");

    g_log.log("\n========== TEST: multi-thread: client resumes worker ==========");
    auto main_thread = thread_id_str();
    g_log.log("[TEST] Main thread: %", main_thread);

    std::atomic<bool> done{false};
    std::atomic<int> result{0};
    std::string client_thread_id;

    // Run client in separate thread
    std::thread client_thread([&]() {
        client_thread_id = thread_id_str();
        g_log.log("[CLIENT_THREAD] Started, thread=%", client_thread_id);

        auto future = send(client.get(), address_t::empty_address(), &client_actor::process, 21);
        g_log.log("[CLIENT_THREAD] Sent process(21)");

        // 1. Resume client - suspend на co_await
        client->resume(1);
        g_log.log("[CLIENT_THREAD] After client resume, has_pending=%", client->has_pending());

        // 2. Resume worker - обработает compute
        worker->resume(1);
        g_log.log("[CLIENT_THREAD] After worker resume");

        // 3. Poll pending - resume корутину
        client->poll_pending();
        g_log.log("[CLIENT_THREAD] After poll_pending, future.is_ready()=%", future.is_ready());

        REQUIRE(future.is_ready());
        result = std::move(future).get();
        g_log.log("[CLIENT_THREAD] result=%", result.load());

        done = true;
    });

    client_thread.join();

    REQUIRE(done);
    REQUIRE(result == 52);

    // Client's coroutine runs in client_thread
    g_log.log("[TEST] client_thread_id=%", client_thread_id);
    g_log.log("[TEST] client->process_start_thread()=%", client->process_start_thread());
    g_log.log("[TEST] worker->last_compute_thread()=%", worker->last_compute_thread());

    REQUIRE(client->process_start_thread() == client_thread_id);
    REQUIRE(client->process_after_await_thread() == client_thread_id);
    REQUIRE(client->process_end_thread() == client_thread_id);

    // Worker also runs in client_thread
    REQUIRE(worker->last_compute_thread() == client_thread_id);

    g_log.log("========== TEST PASSED ==========");
}


TEST_CASE("multi-thread: two clients in parallel threads (separate workers)") {
    auto* resource = pmr::get_default_resource();
    // Each client has its OWN worker (no shared state, no race condition)
    auto worker1 = spawn<worker_actor>(resource, "Worker1");
    auto worker2 = spawn<worker_actor>(resource, "Worker2");
    auto client1 = spawn<client_actor>(resource, worker1->address(), "Client1");
    auto client2 = spawn<client_actor>(resource, worker2->address(), "Client2");

    g_log.log("\n========== TEST: multi-thread: two clients in parallel (separate workers) ==========");
    auto main_thread = thread_id_str();
    g_log.log("[TEST] Main thread: %", main_thread);

    std::atomic<int> result1{0};
    std::atomic<int> result2{0};
    std::string thread1_id, thread2_id;

    std::thread t1([&]() {
        thread1_id = thread_id_str();
        g_log.log("[THREAD1] Started, thread=%", thread1_id);

        auto future = send(client1.get(), address_t::empty_address(), &client_actor::process, 10);
        client1->resume(1);
        worker1->resume(1);
        client1->poll_pending();

        REQUIRE(future.is_ready());
        result1 = std::move(future).get();
        g_log.log("[THREAD1] result1=%", result1.load());
    });

    std::thread t2([&]() {
        thread2_id = thread_id_str();
        g_log.log("[THREAD2] Started, thread=%", thread2_id);

        auto future = send(client2.get(), address_t::empty_address(), &client_actor::process, 20);
        client2->resume(1);
        worker2->resume(1);
        client2->poll_pending();

        REQUIRE(future.is_ready());
        result2 = std::move(future).get();
        g_log.log("[THREAD2] result2=%", result2.load());
    });

    t1.join();
    t2.join();

    REQUIRE(result1 == 30);  // 10 * 2 + 10
    REQUIRE(result2 == 50);  // 20 * 2 + 10

    // Each client runs in its own thread
    g_log.log("[TEST] thread1_id=%", thread1_id);
    g_log.log("[TEST] thread2_id=%", thread2_id);
    g_log.log("[TEST] client1->process_start_thread()=%", client1->process_start_thread());
    g_log.log("[TEST] client2->process_start_thread()=%", client2->process_start_thread());

    REQUIRE(client1->process_start_thread() == thread1_id);
    REQUIRE(client1->process_end_thread() == thread1_id);
    REQUIRE(client2->process_start_thread() == thread2_id);
    REQUIRE(client2->process_end_thread() == thread2_id);

    // Threads are different
    REQUIRE(thread1_id != thread2_id);

    // Workers run in their respective client threads (inline execution)
    REQUIRE(worker1->last_compute_thread() == thread1_id);
    REQUIRE(worker2->last_compute_thread() == thread2_id);

    g_log.log("========== TEST PASSED ==========");
}


TEST_CASE("multi-thread: verify coroutine thread affinity") {
    auto* resource = pmr::get_default_resource();
    auto worker = spawn<worker_actor>(resource, "Worker");
    auto client = spawn<client_actor>(resource, worker->address(), "Client");

    g_log.log("\n========== TEST: multi-thread: verify coroutine thread affinity ==========");
    auto main_thread = thread_id_str();
    g_log.log("[TEST] Main thread: %", main_thread);

    std::string worker_thread_id;
    std::atomic<bool> worker_ready{false};
    std::atomic<bool> stop_worker{false};

    // Start worker in separate thread (but we won't use it for inline execution)
    // This is to verify that inline execution happens in CLIENT thread, not worker thread
    std::thread worker_thread([&]() {
        worker_thread_id = thread_id_str();
        g_log.log("[WORKER_THREAD] Started, thread=%", worker_thread_id);
        worker_ready = true;

        // Just wait (worker won't be resumed from this thread in inline execution model)
        while (!stop_worker) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        g_log.log("[WORKER_THREAD] Stopped");
    });

    while (!worker_ready) {
        std::this_thread::yield();
    }

    std::string client_thread_id;
    std::atomic<int> result{0};

    // Client in its own thread
    std::thread client_thread([&]() {
        client_thread_id = thread_id_str();
        g_log.log("[CLIENT_THREAD] Started, thread=%", client_thread_id);

        auto future = send(client.get(), address_t::empty_address(), &client_actor::process, 21);
        g_log.log("[CLIENT_THREAD] Sent process(21)");

        // Client resumes worker in CLIENT thread
        client->resume(1);
        worker->resume(1);
        client->poll_pending();

        REQUIRE(future.is_ready());
        result = std::move(future).get();
        g_log.log("[CLIENT_THREAD] result=%", result.load());
    });

    client_thread.join();
    stop_worker = true;
    worker_thread.join();

    REQUIRE(result == 52);

    g_log.log("[TEST] main_thread=%", main_thread);
    g_log.log("[TEST] worker_thread_id=%", worker_thread_id);
    g_log.log("[TEST] client_thread_id=%", client_thread_id);
    g_log.log("[TEST] client->process_start_thread()=%", client->process_start_thread());
    g_log.log("[TEST] worker->last_compute_thread()=%", worker->last_compute_thread());

    // Coroutine starts and ends in CLIENT thread
    REQUIRE(client->process_start_thread() == client_thread_id);
    REQUIRE(client->process_after_await_thread() == client_thread_id);
    REQUIRE(client->process_end_thread() == client_thread_id);

    // Worker::compute() also runs in CLIENT thread (inline execution!)
    // NOT in worker_thread!
    REQUIRE(worker->last_compute_thread() == client_thread_id);
    REQUIRE(worker->last_compute_thread() != worker_thread_id);

    g_log.log("========== TEST PASSED ==========");
}


TEST_CASE("multi-thread: many iterations (each thread has own worker)") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: multi-thread: many iterations ==========");

    constexpr int NUM_ITERATIONS = 10;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        threads.emplace_back([resource, &success_count, i]() {
            auto tid = thread_id_str();
            g_log.log("[THREAD %] Started, thread=%", i, tid);

            // Each thread creates its OWN worker and client (no shared state)
            auto local_worker = spawn<worker_actor>(resource, "Worker" + std::to_string(i));
            auto local_client = spawn<client_actor>(resource, local_worker->address(), "Client" + std::to_string(i));

            auto future = send(local_client.get(), address_t::empty_address(), &client_actor::process, (i + 1) * 10);
            local_client->resume(1);
            local_worker->resume(1);
            local_client->poll_pending();

            if (future.is_ready()) {
                int result = std::move(future).get();
                int expected = (i + 1) * 10 * 2 + 10;
                g_log.log("[THREAD %] result=% expected=%", i, result, expected);

                if (result == expected) {
                    ++success_count;
                }

                // Verify thread affinity
                if (local_client->process_start_thread() == tid &&
                    local_client->process_end_thread() == tid &&
                    local_worker->last_compute_thread() == tid) {
                    g_log.log("[THREAD %] Thread affinity OK", i);
                } else {
                    g_log.log("[THREAD %] !!! Thread affinity FAILED !!!", i);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    g_log.log("[TEST] success_count=%/%", success_count.load(), NUM_ITERATIONS);
    REQUIRE(success_count == NUM_ITERATIONS);

    g_log.log("========== TEST PASSED ==========");
}