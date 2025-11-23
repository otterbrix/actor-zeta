#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/dispatch.hpp>
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
        : basic_actor<worker_actor>(resource) {
    }

    ~worker_actor() = default;

    /// @brief Simple computation - returns input * 2
    /// @note Records the thread ID where this runs
    unique_future<int> compute(int x) {
        worker_thread_id_.store(std::this_thread::get_id());
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

    std::thread::id worker_thread_id() const {
        return worker_thread_id_.load();
    }

private:
    std::atomic<std::thread::id> worker_thread_id_{};
};

/// @brief Client actor that uses co_await to call worker
class client_actor final : public basic_actor<client_actor> {
public:
    explicit client_actor(pmr::memory_resource* resource, worker_actor* worker, scheduler::sharing_scheduler* scheduler)
        : basic_actor<client_actor>(resource)
        , worker_(worker)
        , scheduler_(scheduler) {
    }

    ~client_actor() = default;

    /// @brief Coroutine method that calls worker and checks thread
    /// @return Tuple of (result, before_thread_id, after_thread_id)
    unique_future<int> process(int x) {
        std::cerr << "[process] START x=" << x << std::endl;

        // Record thread BEFORE co_await
        auto before_thread = std::this_thread::get_id();
        before_thread_id_.store(before_thread);
        std::cerr << "[process] before_thread recorded" << std::endl;

        // Call worker asynchronously
        std::cerr << "[process] Calling send() to worker..." << std::endl;
        auto future = send(worker_, address(), &worker_actor::compute, x);
        std::cerr << "[process] send() returned, needs_scheduling=" << future.needs_scheduling() << std::endl;

        // IMPORTANT: Check if worker needs scheduling and schedule it
        // This is the key fix - we MUST schedule the receiver if it needs it
        if (future.needs_scheduling() && scheduler_) {
            std::cerr << "[process] Enqueueing worker" << std::endl;
            scheduler_->enqueue(worker_);
        }

        // CRITICAL TEST: co_await should suspend and resume in SAME thread
        std::cerr << "[process] BEFORE co_await" << std::endl;
        int result = co_await std::move(future);
        std::cerr << "[process] AFTER co_await, result=" << result << std::endl;

        // Record thread AFTER co_await
        auto after_thread = std::this_thread::get_id();
        after_thread_id_.store(after_thread);

        // Check that we resumed in the SAME thread (client's thread, not worker's)
        if (before_thread != after_thread) {
            thread_mismatch_detected_.store(true);
        }

        std::cerr << "[process] About to co_return result=" << result << std::endl;
        co_return result;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&client_actor::process>;

    unique_future<void> behavior(mailbox::message* msg) {
        std::cerr << "[client_actor::behavior] START, command=" << msg->command() << std::endl;
        switch (msg->command()) {
            case msg_id<client_actor, &client_actor::process>:
                std::cerr << "[client_actor::behavior] Matched process command, calling dispatch()" << std::endl;
                return dispatch(this, &client_actor::process, msg);
            default:
                break;
        }

        std::cerr << "[client_actor::behavior] No match, returning ready void" << std::endl;
        // Note: Actor does NOT re-schedule itself
        // Test will handle scheduling via polling loop when needed
        return make_ready_future_void(resource());
    }

    std::thread::id before_thread_id() const { return before_thread_id_.load(); }
    std::thread::id after_thread_id() const { return after_thread_id_.load(); }
    bool thread_mismatch_detected() const { return thread_mismatch_detected_.load(); }

private:
    worker_actor* worker_;
    scheduler::sharing_scheduler* scheduler_;

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

    int result = 0;

    // Check if we need to schedule the actor
    std::cerr << "[TEST] needs_scheduling = " << result_future.needs_scheduling() << std::endl;

    if (result_future.needs_scheduling()) {
        // Actor needs scheduling - enqueue it
        std::cerr << "[TEST] Enqueueing client actor" << std::endl;
        scheduler->enqueue(client.get());

        // IMPORTANT: After enqueue(), continuation propagates result to message slot
        // We CANNOT call .get() on result_future anymore (it's moved-from)
        // Instead, we wait via polling on is_ready() which works through continuation

        // Polling: Wait until future becomes ready (via continuation propagation)
        auto start = std::chrono::steady_clock::now();
        int iterations = 0;
        while (!result_future.is_ready()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            iterations++;

            // Timeout protection (10 seconds)
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
                std::cerr << "[TEST] TIMEOUT after " << iterations << " iterations" << std::endl;
                FAIL("Timeout waiting for result");
            }
        }

        std::cerr << "[TEST] Future ready after " << iterations << " iterations" << std::endl;
        std::cerr << "[TEST] result_future.valid() = " << result_future.valid() << std::endl;
        std::cerr << "[TEST] result_future.is_ready() = " << result_future.is_ready() << std::endl;
        // Now future is ready - safe to get result
        std::cerr << "[TEST] Calling .get()..." << std::endl;
        result = std::move(result_future).get();
        std::cerr << "[TEST] Got result = " << result << std::endl;
    } else {
        // Actor already processed the message synchronously
        std::cerr << "[TEST] Calling .get() immediately (sync path)" << std::endl;
        result = std::move(result_future).get();
    }

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

    // Wait for all results - clients will keep re-scheduling themselves while coroutines are suspended
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