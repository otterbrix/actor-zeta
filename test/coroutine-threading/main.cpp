#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

using namespace actor_zeta;

std::string thread_id_str() {
    std::ostringstream oss;
    oss << std::this_thread::get_id();
    return oss.str();
}

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

class worker_actor final : public basic_actor<worker_actor> {
public:
    explicit worker_actor(std::pmr::memory_resource* resource, const std::string& name)
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
        co_return x * 2;
    }

    const std::string& last_compute_thread() const { return last_compute_thread_; }
    int compute_count() const { return compute_count_.load(); }
    const std::string& name() const { return name_; }

    using dispatch_traits = actor_zeta::dispatch_traits<&worker_actor::compute>;

    behavior_t behavior(mailbox::message* msg) {
        auto tid = thread_id_str();
        g_log.log("[%::behavior] thread=% command=%", name_, tid, msg->command());
        last_behavior_thread_ = tid;

        switch (msg->command()) {
            case msg_id<worker_actor, &worker_actor::compute>:
                co_await dispatch(this, &worker_actor::compute, msg);
                break;
            default:
                g_log.log("[%::behavior] Unknown command!", name_);
                break;
        }
    }

    const std::string& last_behavior_thread() const { return last_behavior_thread_; }

    ~worker_actor() = default;

private:
    std::string name_;
    std::atomic<int> compute_count_;
    std::string last_compute_thread_;
    std::string last_behavior_thread_;
};


class client_actor final : public basic_actor<client_actor> {
public:
    explicit client_actor(std::pmr::memory_resource* resource, address_t worker_address, const std::string& name)
        : basic_actor<client_actor>(resource)
        , worker_address_(worker_address)
        , name_(name)
        , final_result_(0) {
    }

    bool poll_pending() {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->is_ready()) {
                g_log.log("[%::poll_pending] coroutine completed", name_);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        return !pending_.empty();
    }

    bool has_pending() const { return !pending_.empty(); }

    unique_future<int> process(int x) {
        auto tid_start = thread_id_str();
        g_log.log("\n[%::process] === START === thread=% x=%", name_, tid_start, x);
        process_start_thread_ = tid_start;

        g_log.log("[%::process] Sending to worker...", name_);
        auto [needs_sched, future] = send(worker_address_, &worker_actor::compute, x);
        g_log.log("[%::process] future.is_ready()=%", name_, future.is_ready());

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
        co_return final_result_.load();
    }

    const std::string& process_start_thread() const { return process_start_thread_; }
    const std::string& process_after_await_thread() const { return process_after_await_thread_; }
    const std::string& process_end_thread() const { return process_end_thread_; }
    const std::string& name() const { return name_; }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &client_actor::process,
        &client_actor::get_result
    >;

    behavior_t behavior(mailbox::message* msg) {
        auto tid = thread_id_str();
        g_log.log("[%::behavior] thread=% command=%", name_, tid, msg->command());
        last_behavior_thread_ = tid;

        switch (msg->command()) {
            case msg_id<client_actor, &client_actor::process>:
                co_await dispatch(this, &client_actor::process, msg);
                break;
            case msg_id<client_actor, &client_actor::get_result>:
                co_await dispatch(this, &client_actor::get_result, msg);
                break;
            default:
                g_log.log("[%::behavior] Unknown command!", name_);
                break;
        }
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
    std::vector<unique_future<int>> pending_;
};


namespace {

    // Drive `actor` until it stops asking to be rescheduled, or `cap` turns pass;
    // true means it went idle. No scheduler on purpose: the assertions pin that the
    // coroutine runs inline on the thread that called resume(), so the verdict is
    // the loop condition here. The cap is load-bearing: several staged sequences
    // leave an actor suspended on a producer they have not driven yet, and such an
    // actor reports `resume` forever.
    template<typename Actor>
    bool drive(Actor* actor, size_t max_throughput, int cap = 8) {
        for (int i = 0; i < cap; ++i) {
            if (actor->resume(max_throughput).result != actor_zeta::scheduler::resume_result::resume) {
                return true;
            }
        }
        return false;
    }

    // False means the actor still demands re-scheduling after the staged sequence:
    // the "N resumes are enough" premise is broken. No Catch2 macro here on purpose:
    // some call sites run on worker threads and Catch2 v2 assertions race on their
    // ostringstream, so threaded callers latch the result and assert after join().
    template<typename Actor>
    [[nodiscard]] bool resume_leaves_idle(Actor* actor, size_t max_throughput) {
        return actor->resume(max_throughput).result != actor_zeta::scheduler::resume_result::resume;
    }

} // namespace

TEST_CASE("single-thread: worker only") {
    auto* resource = std::pmr::get_default_resource();
    auto worker = spawn<worker_actor>(resource, "Worker");

    g_log.log("\n========== TEST: single-thread: worker only ==========");
    auto main_thread = thread_id_str();
    g_log.log("[TEST] Main thread: %", main_thread);

    auto [needs_sched, future] = send(worker.get(), &worker_actor::compute, 21);
    REQUIRE(resume_leaves_idle(worker.get(), 1));

    REQUIRE(future.is_ready());
    int result = std::move(future).take_ready();

    REQUIRE(result == 42);
    REQUIRE(worker->last_compute_thread() == main_thread);

    g_log.log("========== TEST PASSED ==========");
}


TEST_CASE("single-thread: client-worker") {
    auto* resource = std::pmr::get_default_resource();
    auto worker = spawn<worker_actor>(resource, "Worker");
    auto client = spawn<client_actor>(resource, worker->address(), "Client");

    g_log.log("\n========== TEST: single-thread: client-worker ==========");
    auto main_thread = thread_id_str();
    g_log.log("[TEST] Main thread: %", main_thread);

    auto [needs_sched, future] = send(client.get(), &client_actor::process, 21);

    // 1. client starts process(), sends to the worker, suspends on the co_await
    drive(client.get(), 1);
    g_log.log("[TEST] After client resume");
    REQUIRE(!future.is_ready());

    // 2. worker answers (flag-only: the client is not resumed by this)
    drive(worker.get(), 1);
    g_log.log("[TEST] After worker resume");

    // 3. client drains the ready await and completes
    REQUIRE(resume_leaves_idle(client.get(), 10));
    g_log.log("[TEST] After second client resume, future.is_ready()=%", future.is_ready());

    REQUIRE(future.is_ready());
    int result = std::move(future).take_ready();

    REQUIRE(result == 52);  // 21 * 2 + 10

    // All on the main thread.
    REQUIRE(client->process_start_thread() == main_thread);
    REQUIRE(client->process_after_await_thread() == main_thread);
    REQUIRE(client->process_end_thread() == main_thread);
    REQUIRE(worker->last_compute_thread() == main_thread);

    g_log.log("========== TEST PASSED ==========");
}


TEST_CASE("multi-thread: client resumes worker in same thread") {
    auto* resource = std::pmr::get_default_resource();
    auto worker = spawn<worker_actor>(resource, "Worker");
    auto client = spawn<client_actor>(resource, worker->address(), "Client");

    g_log.log("\n========== TEST: multi-thread: client resumes worker ==========");
    auto main_thread = thread_id_str();
    g_log.log("[TEST] Main thread: %", main_thread);

    std::atomic<bool> done{false};
    std::atomic<int> result{0};
    std::atomic<bool> future_available{false};
    std::atomic<bool> client_left_idle{false};
    std::string client_thread_id;

    std::thread client_thread([&]() {
        client_thread_id = thread_id_str();
        g_log.log("[CLIENT_THREAD] Started, thread=%", client_thread_id);

        auto [needs_sched, future] = send(client.get(), &client_actor::process, 21);
        g_log.log("[CLIENT_THREAD] Sent process(21)");

        drive(client.get(), 1);
        g_log.log("[CLIENT_THREAD] After client resume");

        drive(worker.get(), 1);
        g_log.log("[CLIENT_THREAD] After worker resume");

        client_left_idle = resume_leaves_idle(client.get(), 10);
        g_log.log("[CLIENT_THREAD] After second client resume, future.is_ready()=%", future.is_ready());

        future_available = future.is_ready();
        if (future_available) {
            result = std::move(future).take_ready();
        }
        g_log.log("[CLIENT_THREAD] result=%", result.load());

        done = true;
    });

    client_thread.join();
    REQUIRE(client_left_idle);

    REQUIRE(done);
    REQUIRE(future_available);
    REQUIRE(result == 52);

    g_log.log("[TEST] client_thread_id=%", client_thread_id);
    g_log.log("[TEST] client->process_start_thread()=%", client->process_start_thread());
    g_log.log("[TEST] worker->last_compute_thread()=%", worker->last_compute_thread());

    REQUIRE(client->process_start_thread() == client_thread_id);
    REQUIRE(client->process_after_await_thread() == client_thread_id);
    REQUIRE(client->process_end_thread() == client_thread_id);

    // The worker ran inline on the client's thread too.
    REQUIRE(worker->last_compute_thread() == client_thread_id);

    g_log.log("========== TEST PASSED ==========");
}


TEST_CASE("multi-thread: two clients in parallel threads (separate workers)") {
    auto* resource = std::pmr::get_default_resource();
    // Each client has its own worker, so the two threads share no actor.
    auto worker1 = spawn<worker_actor>(resource, "Worker1");
    auto worker2 = spawn<worker_actor>(resource, "Worker2");
    auto client1 = spawn<client_actor>(resource, worker1->address(), "Client1");
    auto client2 = spawn<client_actor>(resource, worker2->address(), "Client2");

    g_log.log("\n========== TEST: multi-thread: two clients in parallel (separate workers) ==========");
    auto main_thread = thread_id_str();
    g_log.log("[TEST] Main thread: %", main_thread);

    std::atomic<int> result1{0};
    std::atomic<int> result2{0};
    std::atomic<bool> future1_available{false};
    std::atomic<bool> future2_available{false};
    std::atomic<bool> client1_left_idle{false};
    std::atomic<bool> client2_left_idle{false};
    std::string thread1_id, thread2_id;

    std::thread t1([&]() {
        thread1_id = thread_id_str();
        g_log.log("[THREAD1] Started, thread=%", thread1_id);

        auto [needs_sched, future] = send(client1.get(), &client_actor::process, 10);
        drive(client1.get(), 1);
        drive(worker1.get(), 1);
        client1_left_idle = resume_leaves_idle(client1.get(), 10);

        future1_available = future.is_ready();
        if (future1_available) {
            result1 = std::move(future).take_ready();
        }
        g_log.log("[THREAD1] result1=%", result1.load());
    });

    std::thread t2([&]() {
        thread2_id = thread_id_str();
        g_log.log("[THREAD2] Started, thread=%", thread2_id);

        auto [needs_sched, future] = send(client2.get(), &client_actor::process, 20);
        drive(client2.get(), 1);
        drive(worker2.get(), 1);
        client2_left_idle = resume_leaves_idle(client2.get(), 10);

        future2_available = future.is_ready();
        if (future2_available) {
            result2 = std::move(future).take_ready();
        }
        g_log.log("[THREAD2] result2=%", result2.load());
    });

    t1.join();
    t2.join();

    REQUIRE(client1_left_idle);
    REQUIRE(client2_left_idle);

    REQUIRE(future1_available);
    REQUIRE(future2_available);
    REQUIRE(result1 == 30);  // 10 * 2 + 10
    REQUIRE(result2 == 50);  // 20 * 2 + 10

    g_log.log("[TEST] thread1_id=%", thread1_id);
    g_log.log("[TEST] thread2_id=%", thread2_id);
    g_log.log("[TEST] client1->process_start_thread()=%", client1->process_start_thread());
    g_log.log("[TEST] client2->process_start_thread()=%", client2->process_start_thread());

    REQUIRE(client1->process_start_thread() == thread1_id);
    REQUIRE(client1->process_end_thread() == thread1_id);
    REQUIRE(client2->process_start_thread() == thread2_id);
    REQUIRE(client2->process_end_thread() == thread2_id);

    REQUIRE(thread1_id != thread2_id);

    // Workers ran inline on their client's thread.
    REQUIRE(worker1->last_compute_thread() == thread1_id);
    REQUIRE(worker2->last_compute_thread() == thread2_id);

    g_log.log("========== TEST PASSED ==========");
}


TEST_CASE("multi-thread: verify coroutine thread affinity") {
    auto* resource = std::pmr::get_default_resource();
    auto worker = spawn<worker_actor>(resource, "Worker");
    auto client = spawn<client_actor>(resource, worker->address(), "Client");

    g_log.log("\n========== TEST: multi-thread: verify coroutine thread affinity ==========");
    auto main_thread = thread_id_str();
    g_log.log("[TEST] Main thread: %", main_thread);

    std::string worker_thread_id;
    std::atomic<bool> worker_ready{false};
    std::atomic<bool> stop_worker{false};

    // A thread that never resumes the worker: it exists only so there is a second
    // thread id to assert the worker's compute() did NOT run on.
    std::thread worker_thread([&]() {
        worker_thread_id = thread_id_str();
        g_log.log("[WORKER_THREAD] Started, thread=%", worker_thread_id);
        worker_ready = true;

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
    std::atomic<bool> future_available{false};
    std::atomic<bool> client_left_idle{false};

    std::thread client_thread([&]() {
        client_thread_id = thread_id_str();
        g_log.log("[CLIENT_THREAD] Started, thread=%", client_thread_id);

        auto [needs_sched, future] = send(client.get(), &client_actor::process, 21);
        g_log.log("[CLIENT_THREAD] Sent process(21)");

        drive(client.get(), 1);
        drive(worker.get(), 1);
        client_left_idle = resume_leaves_idle(client.get(), 10);

        future_available = future.is_ready();
        if (future_available) {
            result = std::move(future).take_ready();
        }
        g_log.log("[CLIENT_THREAD] result=%", result.load());
    });

    client_thread.join();
    stop_worker = true;
    worker_thread.join();

    REQUIRE(future_available);
    REQUIRE(result == 52);

    g_log.log("[TEST] main_thread=%", main_thread);
    g_log.log("[TEST] worker_thread_id=%", worker_thread_id);
    g_log.log("[TEST] client_thread_id=%", client_thread_id);
    g_log.log("[TEST] client->process_start_thread()=%", client->process_start_thread());
    g_log.log("[TEST] worker->last_compute_thread()=%", worker->last_compute_thread());

    REQUIRE(client->process_start_thread() == client_thread_id);
    REQUIRE(client->process_after_await_thread() == client_thread_id);
    REQUIRE(client->process_end_thread() == client_thread_id);

    // compute() ran inline on the client's thread, not on worker_thread.
    REQUIRE(worker->last_compute_thread() == client_thread_id);
    REQUIRE(worker->last_compute_thread() != worker_thread_id);

    g_log.log("========== TEST PASSED ==========");
}


TEST_CASE("multi-thread: many iterations (each thread has own worker)") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: multi-thread: many iterations ==========");

    constexpr int NUM_ITERATIONS = 10;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    std::atomic<int> left_idle_count{0};

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        threads.emplace_back([resource, &success_count, &left_idle_count, i]() {
            auto tid = thread_id_str();
            g_log.log("[THREAD %] Started, thread=%", i, tid);

            auto local_worker = spawn<worker_actor>(resource, "Worker" + std::to_string(i));
            auto local_client = spawn<client_actor>(resource, local_worker->address(), "Client" + std::to_string(i));

            auto [needs_sched, future] = send(local_client.get(), &client_actor::process, (i + 1) * 10);
            drive(local_client.get(), 1);
            drive(local_worker.get(), 1);
            if (resume_leaves_idle(local_client.get(), 10)) {
                ++left_idle_count;
            }

            if (future.is_ready()) {
                int result = std::move(future).take_ready();
                int expected = (i + 1) * 10 * 2 + 10;
                g_log.log("[THREAD %] result=% expected=%", i, result, expected);

                if (result == expected) {
                    ++success_count;
                }

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
    REQUIRE(left_idle_count == NUM_ITERATIONS);

    g_log.log("========== TEST PASSED ==========");
}