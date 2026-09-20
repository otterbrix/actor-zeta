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
// needs_sched is sufficient on its own: honouring it, and nothing else, must
// deliver every message.
//
// This file used to claim a race between ~resume_guard() and
// try_schedule_after_enqueue(), with this timeline:
//
//   A: resume() finishes, mailbox empty, try_block() succeeds -> awaiting
//   A: ~resume_guard() reads state_ -> running
//   B: send() unblocks the mailbox, try_schedule_after_enqueue() sees running
//      and "returns false WITHOUT setting the scheduled flag"
//   A: ~resume_guard() CAS(running -> idle)
//   => message in the mailbox, actor idle and unscheduled -> lost
//
// Step B is not what the code does. try_schedule_after_enqueue() ALWAYS sets the
// bit and only suppresses the RETURN VALUE (cooperative_actor.hpp, the CAS then
// `return !is_running(current)`). Both sides read-modify-write the same atomic,
// so its modification order totally orders them and only two outcomes exist:
//
//   sender's CAS first  -> ~resume_guard() observes the bit, preserves it, sets
//                          scheduled_while_running, and resume()'s fix-up turns
//                          `awaiting` into `resume` -> the worker re-enqueues
//   guard's CAS first   -> sender reads {scheduled=0, running=0}, its CAS wins,
//                          and it returns TRUE -> the sender enqueues
//
// So the tests below are guards, not repros. Their point is that a sender which
// honours needs_sched and does NOTHING ELSE is enough. They used to rescue the
// actor with an unconditional scheduler->enqueue() on timeout and then assert
// against a count that subtracted those rescues out -- which made a real strand
// print a WARN and pass. No rescues now: a timeout fails the test.
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
// Guard 1: sequential sender, one message at a time.
// =============================================================================

TEST_CASE("needs_scheduling race: every message is delivered") {
    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 10);
    scheduler->start();

    auto actor = actor_zeta::spawn<scheduling_race_actor>(resource);

    constexpr int NUM_ITERATIONS = 1000;
    constexpr auto WAIT_TIMEOUT = std::chrono::seconds(5);

    int delivered = 0;
    int stranded = 0;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                       &scheduling_race_actor::process, i);
        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        // No rescue enqueue here on purpose: the obligation was discharged above,
        // and a second one would hide exactly the failure this test is for.
        auto start = std::chrono::steady_clock::now();
        while (!future.is_ready()) {
            if (std::chrono::steady_clock::now() - start > WAIT_TIMEOUT) {
                ++stranded;
                break;
            }
            std::this_thread::yield();
        }

        if (future.is_ready()) {
            ++delivered;
        }
    }

    scheduler->stop();

    INFO("delivered " << delivered << " of " << NUM_ITERATIONS
                      << ", stranded " << stranded);
    REQUIRE(stranded == 0);
    REQUIRE(delivered == NUM_ITERATIONS);
}

// =============================================================================
// Guard 2: four senders against a two-worker scheduler, maximum contention on
// the state word.
// =============================================================================

TEST_CASE("needs_scheduling race: high contention stress", "[stress]") {
    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 5);
    scheduler->start();

    auto actor = actor_zeta::spawn<scheduling_race_actor>(resource);

    constexpr int NUM_SENDERS = 4;
    constexpr int MESSAGES_PER_SENDER = 500;
    constexpr int TOTAL = NUM_SENDERS * MESSAGES_PER_SENDER;

    std::atomic<int> total_sent{0};

    std::vector<std::thread> senders;
    senders.reserve(NUM_SENDERS);
    for (int s = 0; s < NUM_SENDERS; ++s) {
        senders.emplace_back([&, s]() {
            for (int i = 0; i < MESSAGES_PER_SENDER; ++i) {
                const int value = s * MESSAGES_PER_SENDER + i;
                auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                               &scheduling_race_actor::process, value);
                if (needs_sched) {
                    scheduler->enqueue(actor.get());
                }
                total_sent.fetch_add(1, std::memory_order_relaxed);
                future.detach();
            }
        });
    }

    for (auto& t : senders) {
        t.join();
    }

    // One global deadline instead of a per-message timeout-and-rescue: the senders
    // are done, so anything still unprocessed is stranded, not merely slow.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (actor->processed_count() < static_cast<std::size_t>(TOTAL) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    scheduler->stop();

    INFO("sent " << total_sent.load() << ", processed " << actor->processed_count());
    REQUIRE(total_sent.load() == TOTAL);
    REQUIRE(actor->processed_count() == static_cast<std::size_t>(TOTAL));
}

// =============================================================================
// Guard 3: a second message aimed at the window while the actor is finishing the
// first -- the interleaving the disproved timeline described.
// =============================================================================

TEST_CASE("needs_scheduling race: second message lands mid-teardown") {
    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(1, 1);
    scheduler->start();

    auto actor = actor_zeta::spawn<scheduling_race_actor>(resource);

    constexpr int NUM_ITERATIONS = 100;
    constexpr auto WAIT_TIMEOUT = std::chrono::seconds(5);

    int stranded = 0;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto [needs_sched1, first] = actor_zeta::send(actor.get(),
                                      &scheduling_race_actor::process, i * 2);
        if (needs_sched1) {
            scheduler->enqueue(actor.get());
        }

        // Let the actor get into resume() and start winding down.
        std::this_thread::sleep_for(std::chrono::microseconds(10));

        auto [needs_sched2, second] = actor_zeta::send(actor.get(),
                                       &scheduling_race_actor::process, i * 2 + 1);
        if (needs_sched2) {
            scheduler->enqueue(actor.get());
        }

        auto start = std::chrono::steady_clock::now();
        while (!first.is_ready() || !second.is_ready()) {
            if (std::chrono::steady_clock::now() - start > WAIT_TIMEOUT) {
                ++stranded;
                break;
            }
            std::this_thread::yield();
        }
    }

    scheduler->stop();

    INFO("stranded pairs: " << stranded
                            << ", processed " << actor->processed_count());
    REQUIRE(stranded == 0);
    REQUIRE(actor->processed_count() == static_cast<std::size_t>(NUM_ITERATIONS) * 2);
}
