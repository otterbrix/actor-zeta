#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <thread>
#include <vector>

// Test actor for intrusive_ptr stress testing (via messages/futures)
class refcount_stress_actor final : public actor_zeta::basic_actor<refcount_stress_actor> {
public:
    explicit refcount_stress_actor(actor_zeta::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<refcount_stress_actor>(resource)
        , increment_behavior_(actor_zeta::make_behavior(resource, this, &refcount_stress_actor::increment))
        , get_value_behavior_(actor_zeta::make_behavior(resource, this, &refcount_stress_actor::get_value)) {
    }

    actor_zeta::unique_future<void> increment(int delta) {
        value_.fetch_add(delta, std::memory_order_relaxed);
        return actor_zeta::make_ready_future_void(resource());
    }

    actor_zeta::unique_future<int> get_value() const {
        return actor_zeta::make_ready_future<int>(resource(), value_.load(std::memory_order_relaxed));
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<refcount_stress_actor, &refcount_stress_actor::increment>) {
            increment_behavior_(msg);
        } else if (cmd == actor_zeta::msg_id<refcount_stress_actor, &refcount_stress_actor::get_value>) {
            get_value_behavior_(msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &refcount_stress_actor::increment,
        &refcount_stress_actor::get_value
    >;

private:
    actor_zeta::behavior_t increment_behavior_;
    actor_zeta::behavior_t get_value_behavior_;
    mutable std::atomic<int> value_{0};
};

// =============================================================================
// Helper: Smart get() with active actor rescheduling
// =============================================================================

/// @brief Smart future.get() that actively reschedules actor to ensure message processing
/// @details Under TSAN, actor may become idle after processing first message and never
///          get rescheduled for subsequent messages (needs_scheduling() returns false).
///          This helper actively reschedules the actor every ~1ms until future is ready.
template<typename T, typename Actor>
T smart_get(typename Actor::template unique_future<T>&& future,
            Actor* actor,
            actor_zeta::scheduler::sharing_scheduler* scheduler) {
    constexpr auto timeout = std::chrono::seconds(10);
    auto start_time = std::chrono::steady_clock::now();

    int stall_iterations = 0;
    constexpr int MAX_STALL = 10;  // Reschedule every ~1ms

    while (!future.is_ready()) {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout) {
            // Timeout - but still try to get (will block or return error)
            break;
        }

        ++stall_iterations;

        // Actively reschedule actor every ~1ms to ensure message processing
        if (stall_iterations >= MAX_STALL) {
            scheduler->enqueue(actor);
            stall_iterations = 0;
        }

        std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    return std::move(future).get();
}

// =============================================================================
// Intrusive Ptr Stress Tests (via message/future refcount operations)
// =============================================================================

TEST_CASE("Refcount Stress 1: Concurrent future creation/destruction") {
    // TEST OBJECTIVE:
    // Stress test refcount operations through rapid future creation/destruction
    //
    // RACE CONDITION SCENARIO:
    // Many threads simultaneously send messages to same actor
    // Each message+future pair increments/decrements refcount
    //
    // DETECTION:
    // - Assert in ref() will catch refcount == 0 (use-after-free)
    // - Assert in deref() will catch refcount underflow
    // - TSan will detect data races on rc_ atomic
    // - ASan will detect double-free if refcount is wrong

    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(8, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<refcount_stress_actor>(resource);

    constexpr int NUM_THREADS = 2;   // Minimal for TSAN
    constexpr int OPERATIONS_PER_THREAD = 3;  // Minimal for TSAN
    std::atomic<int> completed{0};
    std::vector<std::thread> threads;

    // Launch threads that rapidly send messages (creates/destroys futures → ref/deref)
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
                // Send message - creates future → increments refcount
                auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                              &refcount_stress_actor::increment, 1);

                if (future.needs_scheduling()) {
                    scheduler->enqueue(actor.get());
                }

                // Future destroyed here → decrements refcount
                // Concurrent with other threads doing same operations
                completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }

    scheduler->stop();

    // Verification
    REQUIRE(completed.load() == NUM_THREADS * OPERATIONS_PER_THREAD);
}

TEST_CASE("Refcount Stress 2: Future move and copy operations") {
    // TEST OBJECTIVE:
    // Stress test future move semantics and refcount during transfers
    //
    // RACE CONDITION SCENARIO:
    // Move operations transfer ownership without changing refcount
    // But getting result from moved futures can cause issues
    //
    // DETECTION:
    // - Assert in future.get() will catch invalid operations
    // - TSan will detect data races on future internal state

    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<refcount_stress_actor>(resource);

    constexpr int NUM_ITERATIONS = 3;  // Minimal for TSAN
    std::atomic<int> move_count{0};

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        // Create future
        auto future1 = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                        &refcount_stress_actor::get_value);

        if (future1.needs_scheduling()) {
            scheduler->enqueue(actor.get());
        }

        // Move chain
        auto future2 = std::move(future1);  // future1 now invalid
        auto future3 = std::move(future2);  // future2 now invalid

        // Get result from final future
        int result = smart_get<int>(std::move(future3), actor.get(), scheduler.get());
        (void)result;

        move_count.fetch_add(1, std::memory_order_relaxed);
    }

    scheduler->stop();
    REQUIRE(move_count.load() == NUM_ITERATIONS);
}

TEST_CASE("Refcount Stress 3: Concurrent message enqueue and future get") {
    // TEST OBJECTIVE:
    // Concurrent message enqueue while waiting on futures
    //
    // RACE CONDITION SCENARIO:
    // Thread 1: Waits on future.get() (spinning on refcount)
    // Thread 2: Sends new messages (modifies mailbox, refcounts)
    // Thread 3: Destroys futures (decrefs messages)
    //
    // DETECTION:
    // - Assert in ref() catches refcount == 0
    // - TSan catches data races
    // - ASan catches use-after-free

    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<refcount_stress_actor>(resource);

    constexpr int NUM_ITERATIONS = 3;  // Minimal for TSAN
    std::atomic<int> results_received{0};

    std::vector<std::thread> threads;

    // Thread 1: Send messages and wait for results
    threads.emplace_back([&]() {
        for (int i = 0; i < NUM_ITERATIONS; ++i) {
            auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                          &refcount_stress_actor::get_value);
            if (future.needs_scheduling()) {
                scheduler->enqueue(actor.get());
            }

            int result = smart_get<int>(std::move(future), actor.get(), scheduler.get());
            (void)result;
            results_received.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Thread 2: Send fire-and-forget messages (create and drop futures)
    threads.emplace_back([&]() {
        for (int i = 0; i < NUM_ITERATIONS; ++i) {
            auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                          &refcount_stress_actor::increment, 1);
            if (future.needs_scheduling()) {
                scheduler->enqueue(actor.get());
            }
            // Future destroyed immediately - orphaned message
        }
    });

    // Thread 3: Rapidly send and destroy futures
    threads.emplace_back([&]() {
        for (int i = 0; i < NUM_ITERATIONS; ++i) {
            {
                auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                              &refcount_stress_actor::increment, -1);
                if (future.needs_scheduling()) {
                    scheduler->enqueue(actor.get());
                }
            }  // Future destroyed here
        }
    });

    for (auto& thread : threads) {
        thread.join();
    }

    scheduler->stop();
    REQUIRE(results_received.load() == NUM_ITERATIONS);
}

TEST_CASE("Refcount Stress 4: Mixed operations stress test") {
    // TEST OBJECTIVE:
    // Mix all operations: send, get, move, orphan futures
    //
    // RACE CONDITION SCENARIO:
    // Maximum chaos - all threads do random operations
    // This creates unpredictable race patterns on refcounts
    //
    // DETECTION:
    // - All asserts in ref_counted (ref/deref)
    // - TSan detects data races
    // - ASan detects memory errors

    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(8, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<refcount_stress_actor>(resource);

    constexpr int NUM_THREADS = 2;   // Minimal for TSAN
    constexpr int OPERATIONS_PER_THREAD = 3;  // Minimal for TSAN
    std::atomic<int> operation_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, thread_id = t]() {
            for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
                int op = (thread_id * OPERATIONS_PER_THREAD + i) % 5;

                switch (op) {
                    case 0: {
                        // Send and immediately destroy (orphan)
                        auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                                      &refcount_stress_actor::increment, 1);
                        if (future.needs_scheduling()) {
                            scheduler->enqueue(actor.get());
                        }
                        break;
                    }
                    case 1: {
                        // Send and wait for result
                        auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                                      &refcount_stress_actor::get_value);
                        if (future.needs_scheduling()) {
                            scheduler->enqueue(actor.get());
                        }
                        int result = smart_get<int>(std::move(future), actor.get(), scheduler.get());
                        (void)result;
                        break;
                    }
                    case 2: {
                        // Send, move, then destroy
                        auto future1 = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                                       &refcount_stress_actor::increment, -1);
                        if (future1.needs_scheduling()) {
                            scheduler->enqueue(actor.get());
                        }
                        auto future2 = std::move(future1);
                        break;
                    }
                    case 3: {
                        // Send, move, get result
                        auto future1 = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                                       &refcount_stress_actor::get_value);
                        if (future1.needs_scheduling()) {
                            scheduler->enqueue(actor.get());
                        }
                        auto future2 = std::move(future1);
                        int result = smart_get<int>(std::move(future2), actor.get(), scheduler.get());
                        (void)result;
                        break;
                    }
                    case 4: {
                        // Multiple moves then destroy
                        auto future1 = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                                       &refcount_stress_actor::increment, 1);
                        if (future1.needs_scheduling()) {
                            scheduler->enqueue(actor.get());
                        }
                        auto future2 = std::move(future1);
                        auto future3 = std::move(future2);
                        break;
                    }
                }

                operation_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }

    scheduler->stop();
    REQUIRE(operation_count.load() == NUM_THREADS * OPERATIONS_PER_THREAD);
}

TEST_CASE("Refcount Stress 5: Actor destruction with pending messages") {
    // TEST OBJECTIVE:
    // Verify refcount correctness when actor is destroyed with pending messages
    //
    // RACE CONDITION SCENARIO:
    // Messages in mailbox still hold refcounts
    // Actor destructor must properly clean up
    //
    // DETECTION:
    // - Assert in ~cooperative_actor catches pending futures
    // - ASan catches memory leaks if messages not cleaned up
    // - TSan catches races during destruction

    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 1000);
    scheduler->start();

    constexpr int NUM_ITERATIONS = 3;  // Minimal for TSAN

    for (int iter = 0; iter < NUM_ITERATIONS; ++iter) {
        // Create new actor for each iteration
        auto actor = actor_zeta::spawn<refcount_stress_actor>(resource);

        // Rapidly send many messages
        constexpr int MESSAGES = 2;  // Minimal for TSAN
        std::vector<refcount_stress_actor::unique_future<int>> futures;
        futures.reserve(MESSAGES);

        for (int i = 0; i < MESSAGES; ++i) {
            auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                          &refcount_stress_actor::get_value);
            if (future.needs_scheduling()) {
                scheduler->enqueue(actor.get());
            }
            futures.push_back(std::move(future));
        }

        // Wait for all futures to complete
        for (auto& future : futures) {
            int result = smart_get<int>(std::move(future), actor.get(), scheduler.get());
            (void)result;
        }

        // Actor destroyed here - verifies refcount cleanup
    }

    scheduler->stop();
}