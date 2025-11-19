#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#if HAVE_STD_COROUTINES
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>

#include <thread>
#include <atomic>
#include <chrono>

using namespace actor_zeta;

// ============================================================================
// Test: Coroutine resumes in CORRECT thread (owner actor's thread, not sender's)
// ============================================================================

/// @brief Worker actor that processes requests in its thread
class worker_actor final : public basic_actor<worker_actor> {
public:
    explicit worker_actor(pmr::memory_resource* resource)
        : basic_actor<worker_actor>(resource)
        , compute_(make_behavior(resource, this, &worker_actor::compute)) {
    }

    ~worker_actor() override {
        begin_shutdown();
    }

    /// @brief Simple computation - returns input * 2
    /// @note Records the thread ID where this runs
    unique_future<int> compute(int x) {
        worker_thread_id_.store(std::this_thread::get_id());
        return x * 2;  // Immediate return
    }

    using dispatch_traits = dispatch_traits<&worker_actor::compute>;

    void behavior(mailbox::message* msg) override {
        switch (msg->command()) {
            case msg_id<worker_actor, &worker_actor::compute>:
                compute_(msg);
                break;
            default:
                break;
        }
    }

    std::thread::id worker_thread_id() const {
        return worker_thread_id_.load();
    }

private:
    behavior_t compute_;
    std::atomic<std::thread::id> worker_thread_id_{};
};

/// @brief Client actor that uses co_await to call worker
class client_actor final : public basic_actor<client_actor> {
public:
    explicit client_actor(pmr::memory_resource* resource, worker_actor* worker)
        : basic_actor<client_actor>(resource)
        , worker_(worker)
        , process_(make_behavior(resource, this, &client_actor::process)) {
    }

    ~client_actor() override {
        begin_shutdown();
    }

    /// @brief Coroutine method that calls worker and checks thread
    /// @return Tuple of (result, before_thread_id, after_thread_id)
    unique_future<int> process(int x) {
        // Record thread BEFORE co_await
        auto before_thread = std::this_thread::get_id();
        before_thread_id_.store(before_thread);

        // Call worker asynchronously
        auto future = send(worker_, address(), &worker_actor::compute, x);

        // CRITICAL TEST: co_await should suspend and resume in SAME thread
        int result = co_await future;

        // Record thread AFTER co_await
        auto after_thread = std::this_thread::get_id();
        after_thread_id_.store(after_thread);

        // Check that we resumed in the SAME thread (client's thread, not worker's)
        if (before_thread != after_thread) {
            thread_mismatch_detected_.store(true);
        }

        co_return result;
    }

    using dispatch_traits = dispatch_traits<&client_actor::process>;

    void behavior(mailbox::message* msg) override {
        // Resume suspended coroutines BEFORE processing new messages
        resume_all(process_);

        switch (msg->command()) {
            case msg_id<client_actor, &client_actor::process>:
                process_(msg);
                break;
            default:
                break;
        }
    }

    std::thread::id before_thread_id() const { return before_thread_id_.load(); }
    std::thread::id after_thread_id() const { return after_thread_id_.load(); }
    bool thread_mismatch_detected() const { return thread_mismatch_detected_.load(); }

private:
    worker_actor* worker_;
    behavior_t process_;

    std::atomic<std::thread::id> before_thread_id_{};
    std::atomic<std::thread::id> after_thread_id_{};
    std::atomic<bool> thread_mismatch_detected_{false};
};

TEST_CASE("coroutine resumes in owner actor's thread, not sender's thread") {
    auto* resource = pmr::get_default_resource();

    // Create scheduler with 2 worker threads
    auto scheduler = std::make_unique<scheduler::sharing_scheduler>(2, 10000);
    scheduler->start();

    // Create actors
    auto worker = spawn<worker_actor>(resource);
    auto client = spawn<client_actor>(resource, worker.get());

    // Attach to scheduler
    scheduler->schedule(worker.get());
    scheduler->schedule(client.get());

    // Send request from client to worker via coroutine
    auto result_future = send(client.get(), address_t{}, &client_actor::process, 21);

    // Wait for result with timeout
    int result = std::move(result_future).get();

    // Give scheduler time to finish processing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ASSERTIONS
    REQUIRE(result == 42);  // 21 * 2 = 42

    // Critical check: coroutine must resume in CLIENT's thread, not WORKER's thread
    auto client_before = client->before_thread_id();
    auto client_after = client->after_thread_id();
    auto worker_thread = worker->worker_thread_id();

    INFO("Client before co_await: " << client_before);
    INFO("Client after co_await:  " << client_after);
    INFO("Worker thread:          " << worker_thread);

    // Main assertion: coroutine resumed in SAME thread (client's thread)
    REQUIRE(client_before == client_after);
    REQUIRE_FALSE(client->thread_mismatch_detected());

    // If this fails, coroutine ran in worker's thread (BUG!)
    // This would happen with old implementation (continuation calls handle.resume() directly)
    REQUIRE(client_after != worker_thread);

    scheduler->stop();
}

TEST_CASE("multiple concurrent coroutines resume in correct threads") {
    auto* resource = pmr::get_default_resource();

    auto scheduler = std::make_unique<scheduler::sharing_scheduler>(4, 10000);
    scheduler->start();

    auto worker = spawn<worker_actor>(resource);
    auto client1 = spawn<client_actor>(resource, worker.get());
    auto client2 = spawn<client_actor>(resource, worker.get());
    auto client3 = spawn<client_actor>(resource, worker.get());

    scheduler->schedule(worker.get());
    scheduler->schedule(client1.get());
    scheduler->schedule(client2.get());
    scheduler->schedule(client3.get());

    // Send concurrent requests
    auto f1 = send(client1.get(), address_t{}, &client_actor::process, 10);
    auto f2 = send(client2.get(), address_t{}, &client_actor::process, 20);
    auto f3 = send(client3.get(), address_t{}, &client_actor::process, 30);

    // Wait for all results
    REQUIRE(std::move(f1).get() == 20);
    REQUIRE(std::move(f2).get() == 40);
    REQUIRE(std::move(f3).get() == 60);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // All clients should have thread consistency
    REQUIRE(client1->before_thread_id() == client1->after_thread_id());
    REQUIRE(client2->before_thread_id() == client2->after_thread_id());
    REQUIRE(client3->before_thread_id() == client3->after_thread_id());

    REQUIRE_FALSE(client1->thread_mismatch_detected());
    REQUIRE_FALSE(client2->thread_mismatch_detected());
    REQUIRE_FALSE(client3->thread_mismatch_detected());

    scheduler->stop();
}

#else // !HAVE_STD_COROUTINES

#include <catch2/catch.hpp>

TEST_CASE("coroutines not supported - skipping tests") {
    WARN("C++20 coroutines not available, skipping threading tests");
    REQUIRE(true);
}

#endif // HAVE_STD_COROUTINES