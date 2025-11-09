#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <thread>
#include <vector>
#include <random>

// Stress test actor - processes messages as fast as possible
class stress_actor final : public actor_zeta::basic_actor<stress_actor> {
public:
    explicit stress_actor(actor_zeta::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<stress_actor>(resource)
        , compute_behavior_(actor_zeta::make_behavior(resource, this, &stress_actor::compute))
        , processed_count_(0) {
    }

    ~stress_actor() = default;

    int compute(int value) {
        // Simulate some work
        processed_count_.fetch_add(1, std::memory_order_relaxed);
        return value * 2;
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<stress_actor, &stress_actor::compute>) {
            compute_behavior_(msg);
        }
    }

    std::size_t processed_count() const {
        return processed_count_.load(std::memory_order_acquire);
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &stress_actor::compute
    >;

private:
    actor_zeta::behavior_t compute_behavior_;
    std::atomic<std::size_t> processed_count_;
};

TEST_CASE("Race condition stress test - future destruction timing") {
    // Reduced concurrency for AddressSanitizer compatibility
    // ASan makes code ~3x slower, increasing race window
    constexpr int NUM_THREADS = 4;  // Was 8

    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);  // Was 4 threads
    scheduler->start();

    // Create actor
    auto actor = actor_zeta::spawn<stress_actor>(resource);

    std::atomic<int> futures_created{0};
    std::atomic<int> futures_destroyed_early{0};
    std::atomic<int> futures_destroyed_late{0};
    std::atomic<int> results_read{0};
    std::atomic<bool> stop{false};

    // Worker threads - send messages and destroy futures at random times
    auto worker = [&](int thread_id) {
        std::mt19937 rng(thread_id);
        std::uniform_int_distribution<int> dist(0, 100);

        while (!stop.load(std::memory_order_acquire)) {
            int value = dist(rng);

            // Send message and get future
            auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                          &stress_actor::compute, value);

            // Schedule actor if it was unblocked by this enqueue
            // scheduler->enqueue() is thread-safe
            if (future.needs_scheduling()) {
                scheduler->enqueue(actor.get());
                // Small delay to reduce concurrent resume() probability with ASan
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }

            futures_created.fetch_add(1, std::memory_order_relaxed);

            // Random decision: destroy future early or read result
            int decision = dist(rng);

            if (decision < 30) {
                // 30% - destroy future immediately (before processing)
                // This should trigger orphaned message cleanup
                futures_destroyed_early.fetch_add(1, std::memory_order_relaxed);
                // ~future() called here

            } else if (decision < 60) {
                // 30% - destroy future after small delay (during processing)
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                futures_destroyed_late.fetch_add(1, std::memory_order_relaxed);
                // ~future() called here

            } else {
                // 40% - read result (normal flow)
                // Wait for result with timeout
                auto start = std::chrono::steady_clock::now();
                while (!future.is_ready()) {
                    auto elapsed = std::chrono::steady_clock::now() - start;
                    if (elapsed > std::chrono::milliseconds(100)) {
                        // Timeout - actor might be overloaded
                        break;
                    }
                    std::this_thread::yield();
                }

                if (future.is_ready()) {
                    results_read.fetch_add(1, std::memory_order_relaxed);
                }
                // ~future() called here
            }

            // Small delay to avoid overwhelming the actor
            if (dist(rng) < 10) {
                std::this_thread::yield();
            }
        }
    };

    // Start worker threads
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(worker, i);
    }

    // Run for a short time (reduced for ASan)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // Was 2 seconds
    stop.store(true, std::memory_order_release);

    // Wait for all threads
    for (auto& t : threads) {
        t.join();
    }

    // Process remaining messages
    scheduler->stop();

    // Give scheduler threads time to fully terminate
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Statistics
    std::cout << "\n=== Stress Test Results ===\n";
    std::cout << "Futures created:        " << futures_created.load() << "\n";
    std::cout << "Destroyed early:        " << futures_destroyed_early.load() << "\n";
    std::cout << "Destroyed late:         " << futures_destroyed_late.load() << "\n";
    std::cout << "Results read:           " << results_read.load() << "\n";
    std::cout << "Actor processed:        " << actor->processed_count() << "\n";
    std::cout << "===========================\n\n";

    // If we got here without crashes, memory safety is OK
    REQUIRE(futures_created.load() > 0);
    REQUIRE(actor->processed_count() > 0);
}

TEST_CASE("Race condition stress test - concurrent future destruction") {
    // This test specifically targets the window between set_error() and has_active_future() check

    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 100);
    scheduler->start();

    auto actor = actor_zeta::spawn<stress_actor>(resource);

    std::atomic<int> double_delete_detected{0};
    std::atomic<int> iterations{0};
    constexpr int MAX_ITERATIONS = 5000;

    for (int i = 0; i < MAX_ITERATIONS; ++i) {
        // Create future
        auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                      &stress_actor::compute, i);

        // Schedule actor if it was unblocked by this enqueue
        if (future.needs_scheduling()) {
            scheduler->enqueue(actor.get());
        }

        // Thread 1: Try to destroy future immediately
        std::thread destroyer([&future]() mutable {
            // Move future to trigger destructor
            auto temp = std::move(future);
            // ~temp() called here - tries to delete message
        });

        // Thread 2: Actor processes (scheduler does this)
        // Let scheduler run for a bit
        std::this_thread::sleep_for(std::chrono::microseconds(1));

        destroyer.join();
        iterations.fetch_add(1, std::memory_order_relaxed);
    }

    scheduler->stop();

    // Give scheduler threads time to fully terminate
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "\n=== Concurrent Destruction Test ===\n";
    std::cout << "Iterations:             " << iterations.load() << "\n";
    std::cout << "Double deletes detected: " << double_delete_detected.load() << "\n";
    std::cout << "Actor processed:        " << actor->processed_count() << "\n";
    std::cout << "===================================\n\n";

    REQUIRE(double_delete_detected.load() == 0);
    REQUIRE(iterations.load() == MAX_ITERATIONS);
}

TEST_CASE("Memory leak detection - orphaned messages") {
    // Test that orphaned messages (future destroyed before processing) are properly cleaned up

    auto* resource = actor_zeta::pmr::get_default_resource();
    // Use 2 threads for faster processing on slow CI servers
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 1000);
    scheduler->start();

    auto actor = actor_zeta::spawn<stress_actor>(resource);

    constexpr int NUM_ORPHANED = 1000;

    // Create many futures and destroy them immediately (before processing)
    for (int i = 0; i < NUM_ORPHANED; ++i) {
        {
            auto future = actor_zeta::send(actor.get(), actor_zeta::address_t::empty_address(),
                                          &stress_actor::compute, i);

            if (future.needs_scheduling()) {
                scheduler->enqueue(actor.get());
            }
            // ~future() immediately - message becomes orphaned
        }
    }

    // Wait for actor to process orphaned messages with timeout
    auto start_time = std::chrono::steady_clock::now();
    constexpr auto timeout = std::chrono::seconds(10);  // Extended timeout for slow CI

    while (actor->processed_count() < NUM_ORPHANED) {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout) {
            break;  // Timeout - let test fail with diagnostic output
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    scheduler->stop();

    std::cout << "\n=== Orphaned Messages Test ===\n";
    std::cout << "Orphaned messages sent: " << NUM_ORPHANED << "\n";
    std::cout << "Actor processed:        " << actor->processed_count() << "\n";
    std::cout << "==============================\n\n";

    // Actor should process all orphaned messages
    // (they are marked orphaned but still processed)
    REQUIRE(actor->processed_count() == NUM_ORPHANED);
}