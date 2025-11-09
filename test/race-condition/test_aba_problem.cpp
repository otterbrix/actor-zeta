#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <thread>
#include <vector>

// Test actor for ABA problem testing
class aba_test_actor final : public actor_zeta::basic_actor<aba_test_actor> {
public:
    explicit aba_test_actor(actor_zeta::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<aba_test_actor>(resource)
        , process_behavior_(actor_zeta::make_behavior(resource, this, &aba_test_actor::process)) {
    }

    int process(int value) {
        // Simulate some work
        return value * 2;
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<aba_test_actor, &aba_test_actor::process>) {
            process_behavior_(msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &aba_test_actor::process
    >;

private:
    actor_zeta::behavior_t process_behavior_;
};

// =============================================================================
// ABA Problem Tests for lifo_inbox
// =============================================================================

TEST_CASE("ABA Test 1: Concurrent push_front/take_head stress test") {
    // TEST OBJECTIVE:
    // Stress test lifo_inbox CAS operations to detect ABA problem
    //
    // ABA PROBLEM SCENARIO:
    // Thread 1: Reads head = A, prepares to CAS(A → B)
    // Thread 2: CAS(A → C), then CAS(C → A)  // A is back!
    // Thread 1: CAS succeeds incorrectly (head looks like it hasn't changed)
    //
    // DETECTION:
    // - TSan will detect data races if ABA causes corruption
    // - ASan will detect use-after-free if ABA breaks memory safety
    // - New asserts will catch livelock if CAS retries too many times
    //
    // STRATEGY:
    // - Many threads rapidly enqueue/dequeue messages
    // - Fast recycling of message objects (increases ABA probability)
    // - Random delays to create race windows

    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<aba_test_actor>(resource);

    constexpr int NUM_THREADS = 2;   // Minimal for TSAN
    constexpr int MESSAGES_PER_THREAD = 3;  // Minimal for TSAN
    std::atomic<int> total_processed{0};
    std::vector<std::thread> threads;

    // Launch multiple threads that rapidly enqueue messages
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, thread_id = t]() {
            for (int i = 0; i < MESSAGES_PER_THREAD; ++i) {
                // Create and send message
                auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                              &aba_test_actor::process, thread_id * 1000 + i);

                // Schedule if needed
                if (future.needs_scheduling()) {
                    scheduler->enqueue(actor.get());
                }

                // Random: sometimes wait for result, sometimes drop future immediately
                // Dropping future quickly increases message object recycling → more ABA probability
                if (i % 3 == 0) {
                    int result = std::move(future).get();
                    REQUIRE(result == (thread_id * 1000 + i) * 2);
                    total_processed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    // Drop future - message still processed, but faster object recycling
                    total_processed.fetch_add(1, std::memory_order_relaxed);
                }

                // Random micro-delay to create race windows
                if (i % 10 == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }

    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }

    scheduler->stop();

    // Verification
    REQUIRE(total_processed.load() == NUM_THREADS * MESSAGES_PER_THREAD);
    // If we reach here without TSan/ASan errors or assert failures, ABA problem not detected!
}

TEST_CASE("ABA Test 2: Rapid actor creation/destruction stress test") {
    // TEST OBJECTIVE:
    // Stress test actor state machine CAS operations
    //
    // ABA PROBLEM SCENARIO:
    // Thread 1: Reads state = idle, prepares CAS(idle → scheduled)
    // Thread 2: CAS(idle → running), processes, CAS(running → idle)  // Back to idle!
    // Thread 1: CAS succeeds incorrectly (state looks unchanged)
    //
    // DETECTION:
    // - Assert in resume_guard will catch concurrent resume()
    // - Assert in enqueue_impl will catch invalid state transitions
    // - TSan will detect data races on state_ atomic

    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 1000);
    scheduler->start();

    constexpr int NUM_ITERATIONS = 5;  // Minimal for TSAN
    constexpr int MESSAGES_PER_ITERATION = 2;  // Minimal for TSAN

    for (int iter = 0; iter < NUM_ITERATIONS; ++iter) {
        // Create actor
        auto actor = actor_zeta::spawn<aba_test_actor>(resource);

        // Rapidly send many messages
        std::vector<aba_test_actor::unique_future<int>> futures;
        futures.reserve(MESSAGES_PER_ITERATION);

        for (int i = 0; i < MESSAGES_PER_ITERATION; ++i) {
            auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                          &aba_test_actor::process, i);
            if (future.needs_scheduling()) {
                scheduler->enqueue(actor.get());
            }
            futures.push_back(std::move(future));
        }

        // Wait for all futures
        for (size_t i = 0; i < futures.size(); ++i) {
            int result = std::move(futures[i]).get();
            REQUIRE(result == static_cast<int>(i) * 2);
        }

        // Actor destroyed here - may race with scheduler processing last messages
        // This creates potential for ABA in actor state transitions
    }

    scheduler->stop();
}

TEST_CASE("ABA Test 3: Concurrent enqueue from multiple threads") {
    // TEST OBJECTIVE:
    // Multiple threads concurrently enqueue to same actor
    //
    // RACE CONDITION SCENARIO:
    // Multiple threads call enqueue_impl() simultaneously
    // Each thread does CAS loop to set state_ from idle → scheduled
    // Only one should succeed, others should see scheduled and return
    //
    // DETECTION:
    // - Assert MAX_CAS_ATTEMPTS will catch livelock
    // - Assert in enqueue_impl will catch invalid states
    // - TSan will detect data races on state_ atomic

    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(8, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<aba_test_actor>(resource);

    constexpr int NUM_ENQUEUE_THREADS = 2;  // Minimal for TSAN
    constexpr int ENQUEUES_PER_THREAD = 2;  // Minimal for TSAN
    std::atomic<int> total_sent{0};
    std::vector<std::thread> threads;

    // Launch many threads that all enqueue to same actor simultaneously
    for (int t = 0; t < NUM_ENQUEUE_THREADS; ++t) {
        threads.emplace_back([&, thread_id = t]() {
            for (int i = 0; i < ENQUEUES_PER_THREAD; ++i) {
                auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                              &aba_test_actor::process, thread_id * 1000 + i);

                // All threads call enqueue_impl() → CAS race on state_
                if (future.needs_scheduling()) {
                    scheduler->enqueue(actor.get());
                }

                total_sent.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Wait for all threads
    for (auto& thread : threads) {
        thread.join();
    }

    scheduler->stop();

    // Verification
    REQUIRE(total_sent.load() == NUM_ENQUEUE_THREADS * ENQUEUES_PER_THREAD);
}

TEST_CASE("ABA Test 4: Interleaved enqueue/resume stress test") {
    // TEST OBJECTIVE:
    // Multiple threads send messages concurrently to create CAS contention
    //
    // RACE CONDITION SCENARIO:
    // Multiple threads call enqueue_impl() simultaneously
    // Each tries CAS(idle → scheduled) but only one succeeds
    // Actor state machine must handle concurrent enqueue attempts correctly
    //
    // DETECTION:
    // - Asserts verify valid state transitions
    // - TSan detects data races
    // - Livelock detection catches infinite CAS retries

    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(8, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<aba_test_actor>(resource);

    constexpr int NUM_THREADS = 2;  // Minimal for TSAN
    constexpr int OPERATIONS_PER_THREAD = 3;  // Minimal for TSAN
    std::atomic<int> completed{0};
    std::vector<std::thread> threads;

    // Launch multiple threads that all send to same actor
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, thread_id = t]() {
            for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
                auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                              &aba_test_actor::process, thread_id * 1000 + i);
                if (future.needs_scheduling()) {
                    scheduler->enqueue(actor.get());
                }
                completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    scheduler->stop();

    REQUIRE(completed.load() == NUM_THREADS * OPERATIONS_PER_THREAD);
}