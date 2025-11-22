#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

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

    ~worker_actor() = default;

    /// @brief Simple computation - returns input * 2
    /// @note Records the thread ID where this runs
    unique_future<int> compute(int x) {
        worker_thread_id_.store(std::this_thread::get_id());
        return make_ready_future(resource(), x * 2);
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&worker_actor::compute>;

    void behavior(mailbox::message* msg) {
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
    explicit client_actor(pmr::memory_resource* resource, worker_actor* worker, scheduler::sharing_scheduler* scheduler)
        : basic_actor<client_actor>(resource)
        , worker_(worker)
        , scheduler_(scheduler)
        , process_(make_behavior(resource, this, &client_actor::process)) {
    }

    ~client_actor() = default;

    /// @brief Coroutine method that calls worker and checks thread
    /// @return Tuple of (result, before_thread_id, after_thread_id)
    unique_future<int> process(int x) {
        // Record thread BEFORE co_await
        auto before_thread = std::this_thread::get_id();
        before_thread_id_.store(before_thread);

        // Call worker asynchronously
        auto future = send(worker_, address(), &worker_actor::compute, x);

        // IMPORTANT: Check if worker needs scheduling and schedule it
        // This is the key fix - we MUST schedule the receiver if it needs it
        if (future.needs_scheduling() && scheduler_) {
            scheduler_->enqueue(worker_);
        }

        // CRITICAL TEST: co_await should suspend and resume in SAME thread
        int result = co_await std::move(future);

        // Record thread AFTER co_await
        auto after_thread = std::this_thread::get_id();
        after_thread_id_.store(after_thread);

        // Check that we resumed in the SAME thread (client's thread, not worker's)
        if (before_thread != after_thread) {
            thread_mismatch_detected_.store(true);
        }

        co_return result;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&client_actor::process>;

    void behavior(mailbox::message* msg) {
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
    scheduler::sharing_scheduler* scheduler_;
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
    auto client = spawn<client_actor>(resource, worker.get(), scheduler.get());

    // Send request from client to worker via coroutine
    auto result_future = send(client.get(), address_t::empty_address(), &client_actor::process, 21);

    // Schedule client to process the message (only if needed)
    if (result_future.needs_scheduling()) {
        scheduler->enqueue(client.get());
    }

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
    auto client1 = spawn<client_actor>(resource, worker.get(), scheduler.get());
    auto client2 = spawn<client_actor>(resource, worker.get(), scheduler.get());
    auto client3 = spawn<client_actor>(resource, worker.get(), scheduler.get());

    // Send concurrent requests
    auto f1 = send(client1.get(), address_t::empty_address(), &client_actor::process, 10);
    if (f1.needs_scheduling()) {
        scheduler->enqueue(client1.get());
    }

    auto f2 = send(client2.get(), address_t::empty_address(), &client_actor::process, 20);
    if (f2.needs_scheduling()) {
        scheduler->enqueue(client2.get());
    }

    auto f3 = send(client3.get(), address_t::empty_address(), &client_actor::process, 30);
    if (f3.needs_scheduling()) {
        scheduler->enqueue(client3.get());
    }

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