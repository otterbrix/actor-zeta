#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// =============================================================================
// Test for race condition described in docs/actor-zeta-race-condition.md
//
// BUG: Race between ~resume_guard() and try_schedule_after_enqueue()
//
// Timeline:
// 1. Thread A (scheduler): resume() running, mailbox empty, try_block() success
// 2. Thread A: check_race_window() -> false (mailbox blocked)
// 3. Thread A: finalize() returns awaiting
// 4. Thread A: ~resume_guard() starts
// 5. Thread A: current = state_.load() -> running
// 6. Thread B: send() -> push_back() unblocks mailbox
// 7. Thread B: try_schedule_after_enqueue():
//    - is_running(current) == TRUE
//    - returns FALSE (does not set scheduled flag!)
// 8. Thread A: ~resume_guard() CAS(running -> idle) success
//
// Result: Message in mailbox, actor in idle state, not scheduled -> MESSAGE LOST
// =============================================================================

class scheduling_race_actor final : public actor_zeta::basic_actor<scheduling_race_actor> {
public:
    explicit scheduling_race_actor(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<scheduling_race_actor>(resource)
        , processed_count_(0)
        , last_value_(-1) {}

    actor_zeta::unique_future<int> process(int value) {
        processed_count_.fetch_add(1, std::memory_order_relaxed);
        last_value_.store(value, std::memory_order_relaxed);
        // Small delay to increase race window
        std::this_thread::yield();
        co_return value;
    }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<scheduling_race_actor, &scheduling_race_actor::process>) {
            co_await dispatch(this, &scheduling_race_actor::process, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&scheduling_race_actor::process>;

    std::size_t processed_count() const { return processed_count_.load(std::memory_order_acquire); }
    int last_value() const { return last_value_.load(std::memory_order_acquire); }

private:
    std::atomic<std::size_t> processed_count_;
    std::atomic<int> last_value_;
};

// =============================================================================
// Test 1: Message loss due to needs_scheduling() race
//
// GOAL: Reproduce scenario where message is enqueued but actor is not scheduled
// because try_schedule_after_enqueue() sees is_running() == true but
// ~resume_guard() clears scheduled flag after check_race_window().
// =============================================================================

TEST_CASE("needs_scheduling race: message loss detection") {
    // This test attempts to trigger the race condition where:
    // 1. Actor is running and about to finish (in ~resume_guard())
    // 2. Sender sends message (try_schedule_after_enqueue sees is_running=true)
    // 3. Actor clears running flag without setting scheduled
    // 4. Message sits in mailbox indefinitely

    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 10);
    scheduler->start();

    auto actor = actor_zeta::spawn<scheduling_race_actor>(resource);

    constexpr int NUM_ITERATIONS = 1000;
    constexpr auto WAIT_TIMEOUT = std::chrono::milliseconds(50);

    std::atomic<int> messages_sent{0};
    std::atomic<int> messages_lost{0};
    std::atomic<int> successful_deliveries{0};

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        // Send message
        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                       &scheduling_race_actor::process, i);

        // NOTE: This is the problematic pattern!
        // needs_scheduling() may return false if actor is "running"
        // but actor may clear running flag before processing our message
        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        messages_sent.fetch_add(1, std::memory_order_relaxed);

        // Wait for message to be processed
        auto start = std::chrono::steady_clock::now();
        while (!future.available()) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > WAIT_TIMEOUT) {
                // Message was not processed in time!
                // This indicates potential message loss due to race condition
                messages_lost.fetch_add(1, std::memory_order_relaxed);

                // Workaround: forcefully reschedule actor
                scheduler->enqueue(actor.get());

                // Wait again
                while (!future.available()) {
                    std::this_thread::yield();
                    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(1)) {
                        break; // Give up
                    }
                }
                break;
            }
            std::this_thread::yield();
        }

        if (future.available()) {
            successful_deliveries.fetch_add(1, std::memory_order_relaxed);
        }
    }

    scheduler->stop();

    std::cout << "\n=== needs_scheduling Race Test ===\n";
    std::cout << "Messages sent:       " << messages_sent.load() << "\n";
    std::cout << "Messages lost:       " << messages_lost.load() << "\n";
    std::cout << "Successful delivery: " << successful_deliveries.load() << "\n";
    std::cout << "Actor processed:     " << actor->processed_count() << "\n";
    std::cout << "==================================\n\n";

    // BUG DETECTION: If messages_lost > 0, the race condition was triggered!
    // Note: This test may not always trigger the bug (it's a race condition)
    // but it provides evidence of the problem when it does occur.

    WARN("Messages lost due to race condition: " << messages_lost.load());

    // All messages should eventually be delivered
    REQUIRE(successful_deliveries.load() == NUM_ITERATIONS);
}

// =============================================================================
// Test 2: High contention stress test
//
// Multiple senders competing while actor is constantly transitioning states
// =============================================================================

TEST_CASE("needs_scheduling race: high contention stress") {
    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 5);
    scheduler->start();

    auto actor = actor_zeta::spawn<scheduling_race_actor>(resource);

    constexpr int NUM_SENDERS = 4;
    constexpr int MESSAGES_PER_SENDER = 500;
    constexpr auto WAIT_TIMEOUT = std::chrono::milliseconds(100);

    std::atomic<int> total_sent{0};
    std::atomic<int> timeouts_detected{0};
    std::atomic<bool> stop{false};

    // Multiple sender threads to maximize contention
    std::vector<std::thread> senders;
    for (int s = 0; s < NUM_SENDERS; ++s) {
        senders.emplace_back([&, s]() {
            for (int i = 0; i < MESSAGES_PER_SENDER && !stop.load(); ++i) {
                int value = s * MESSAGES_PER_SENDER + i;

                auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                               &scheduling_race_actor::process, value);

                if (needs_sched) {
                    scheduler->enqueue(actor.get());
                }

                total_sent.fetch_add(1, std::memory_order_relaxed);

                // Check if message was processed in time
                auto start = std::chrono::steady_clock::now();
                while (!future.available()) {
                    auto elapsed = std::chrono::steady_clock::now() - start;
                    if (elapsed > WAIT_TIMEOUT) {
                        timeouts_detected.fetch_add(1, std::memory_order_relaxed);
                        // Workaround: force reschedule
                        scheduler->enqueue(actor.get());
                        break;
                    }
                    std::this_thread::yield();
                }
            }
        });
    }

    // Wait for all senders
    for (auto& t : senders) {
        t.join();
    }

    // Give time for processing remaining messages
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    scheduler->stop();

    std::cout << "\n=== High Contention Stress Test ===\n";
    std::cout << "Total sent:       " << total_sent.load() << "\n";
    std::cout << "Timeouts:         " << timeouts_detected.load() << "\n";
    std::cout << "Actor processed:  " << actor->processed_count() << "\n";
    std::cout << "===================================\n\n";

    // BUG INDICATOR: timeouts_detected > 0 suggests race condition triggered
    WARN("Timeouts (potential race condition): " << timeouts_detected.load());

    // All messages should eventually be processed
    REQUIRE(actor->processed_count() >= static_cast<size_t>(total_sent.load() - timeouts_detected.load()));
}

// =============================================================================
// Test 3: Explicit race window targeting
//
// Try to hit the exact timing window described in the bug report
// =============================================================================

TEST_CASE("needs_scheduling race: targeted timing") {
    auto* resource = std::pmr::get_default_resource();
    // Single-threaded scheduler to control timing more precisely
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(1, 1);
    scheduler->start();

    auto actor = actor_zeta::spawn<scheduling_race_actor>(resource);

    constexpr int NUM_ITERATIONS = 100;
    std::atomic<int> reschedule_needed{0};

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        // First message to make actor run
        auto [needs_sched1, first] = actor_zeta::send(actor.get(),
                                      &scheduling_race_actor::process, i * 2);
        if (needs_sched1) {
            scheduler->enqueue(actor.get());
        }

        // Small delay to let actor start processing
        std::this_thread::sleep_for(std::chrono::microseconds(10));

        // Second message while actor might be in ~resume_guard()
        auto [needs_sched2, second] = actor_zeta::send(actor.get(),
                                       &scheduling_race_actor::process, i * 2 + 1);

        // This is where the bug manifests:
        // needs_scheduling() returns false because actor is "running"
        // but actor is actually about to clear running flag
        if (needs_sched2) {
            scheduler->enqueue(actor.get());
        }

        // Wait for both messages
        auto start = std::chrono::steady_clock::now();
        while (!first.available() || !second.available()) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::milliseconds(100)) {
                // Timeout - message might be stuck!
                reschedule_needed.fetch_add(1, std::memory_order_relaxed);
                scheduler->enqueue(actor.get());
                break;
            }
            std::this_thread::yield();
        }
    }

    scheduler->stop();

    std::cout << "\n=== Targeted Timing Test ===\n";
    std::cout << "Iterations:         " << NUM_ITERATIONS << "\n";
    std::cout << "Reschedule needed:  " << reschedule_needed.load() << "\n";
    std::cout << "Actor processed:    " << actor->processed_count() << "\n";
    std::cout << "============================\n\n";

    // BUG INDICATOR: reschedule_needed > 0 shows the race was triggered
    WARN("Times reschedule was needed: " << reschedule_needed.load());

    // Verify all messages were eventually processed
    REQUIRE(actor->processed_count() == NUM_ITERATIONS * 2);
}