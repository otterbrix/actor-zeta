#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// Guards, not repros: a sender that honours needs_sched and does NOTHING ELSE
// must deliver every message. The sender's leave_and_maybe_schedule() and the
// runner's ~resume_guard read-modify-write the same state word, so only two
// orders exist: the sender's CAS lands first and the guard sees the scheduled bit
// and upgrades awaiting -> resume, or the guard's lands first and the sender's
// CAS returns needs_sched == true. No rescue enqueues on timeout: a rescue would
// hide exactly the strand these tests exist to catch.

class scheduling_race_actor final : public actor_zeta::basic_actor<scheduling_race_actor> {
public:
    explicit scheduling_race_actor(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<scheduling_race_actor>(resource)
        , processed_count_(0)
        , last_value_(-1) {}

    actor_zeta::unique_future<int> process(int value) {
        processed_count_.fetch_add(1, std::memory_order_relaxed);
        last_value_.store(value, std::memory_order_relaxed);
        std::this_thread::yield(); // widen the window
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

// A second message aimed at the window while the actor is finishing the first.
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
