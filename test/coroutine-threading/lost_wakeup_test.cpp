#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <test/tooltestsuites/scheduler_test.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Guard for the lost wakeup in cooperative_actor::resume_impl. A behavior that
// re-suspends on a fresh await from inside the Q6 drain is kept off the park path by
// two lines: the is_busy() re-check after cont.resume() (else try_block() parks a
// PENDING await -- verdict `awaiting`, job dropped) and Q6 sitting ABOVE the
// blocked-check (completion is flag-only and a bare enqueue does not unblock the
// inbox, so a parked actor's ready continuation would never drain). Pinned by hand with
// resume(1), no threads or clocks, on the VERDICT of the drain step; plus a [stress]
// soak decided by progress, never by a clock.

using namespace actor_zeta;

namespace {

    const char* to_string(scheduler::resume_result result) noexcept {
        switch (result) {
            case scheduler::resume_result::resume:
                return "resume";
            case scheduler::resume_result::awaiting:
                return "awaiting";
            case scheduler::resume_result::done:
                return "done";
            case scheduler::resume_result::shutdown:
                return "shutdown";
        }
        return "unknown";
    }

    // Shared request/response worker; its future completes flag-only, never scheduling the consumer.
    class producer_actor final : public basic_actor<producer_actor> {
    public:
        explicit producer_actor(std::pmr::memory_resource* resource)
            : basic_actor<producer_actor>(resource) {}

        unique_future<int> produce(int x) {
            co_return x * 2;
        }

        using dispatch_traits = actor_zeta::dispatch_traits<&producer_actor::produce>;

        behavior_t behavior(mailbox::message* msg) {
            switch (msg->command()) {
                case msg_id<producer_actor, &producer_actor::produce>:
                    co_await dispatch(this, &producer_actor::produce, msg);
                    break;
                default:
                    break;
            }
        }

        ~producer_actor() = default;
    };

    // Awaits the producer TWICE. The second await, published after the first
    // continuation was drained inside the entry path, is the one that gets stranded.
    class chained_consumer final : public basic_actor<chained_consumer> {
    public:
        chained_consumer(std::pmr::memory_resource* resource, address_t producer)
            : basic_actor<chained_consumer>(resource)
            , producer_(producer)
            , first_await_done_(false)
            , completed_count_(0)
            , result_(0)
            , first_send_needs_sched_(-1)
            , second_send_needs_sched_(-1) {}

        // Nothing inside the library consumes needs_sched, and a handler holds an
        // address_t and no scheduler -- so it records the obligation here and the
        // driver discharges it.
        unique_future<int> chain(int x) {
            auto [needs_sched_first, first_future] = send(producer_, &producer_actor::produce, x);
            first_send_needs_sched_.store(needs_sched_first ? 1 : 0, std::memory_order_release);

            const int first = co_await std::move(first_future);
            first_await_done_.store(true, std::memory_order_release);

            // Issued from INSIDE the entry-path cont.resume(): resume_impl regains control on a fresh chain.
            auto [needs_sched_second, second_future] = send(producer_, &producer_actor::produce, first);
            second_send_needs_sched_.store(needs_sched_second ? 1 : 0, std::memory_order_release);

            const int second = co_await std::move(second_future);

            result_.store(second, std::memory_order_release);
            completed_count_.fetch_add(1, std::memory_order_acq_rel);
            co_return second;
        }

        using dispatch_traits = actor_zeta::dispatch_traits<&chained_consumer::chain>;

        behavior_t behavior(mailbox::message* msg) {
            switch (msg->command()) {
                case msg_id<chained_consumer, &chained_consumer::chain>:
                    co_await dispatch(this, &chained_consumer::chain, msg);
                    break;
                default:
                    break;
            }
        }

        bool first_await_done() const noexcept {
            return first_await_done_.load(std::memory_order_acquire);
        }

        int first_send_needs_sched() const noexcept {
            return first_send_needs_sched_.load(std::memory_order_acquire);
        }

        int second_send_needs_sched() const noexcept {
            return second_send_needs_sched_.load(std::memory_order_acquire);
        }

        int completed_count() const noexcept {
            return completed_count_.load(std::memory_order_acquire);
        }

        int result() const noexcept {
            return result_.load(std::memory_order_acquire);
        }

        ~chained_consumer() = default;

    private:
        address_t producer_;
        std::atomic<bool> first_await_done_;
        std::atomic<int> completed_count_;
        std::atomic<int> result_;
        std::atomic<int> first_send_needs_sched_;  // -1 = not sent yet, else the reported obligation
        std::atomic<int> second_send_needs_sched_;
    };

    // Soak consumer: records each scheduling obligation for the driver to discharge.
    class soak_consumer final : public basic_actor<soak_consumer> {
    public:
        soak_consumer(std::pmr::memory_resource* resource, address_t producer)
            : basic_actor<soak_consumer>(resource)
            , producer_(producer)
            , completed_count_(0)
            , producer_obligations_(0) {}

        unique_future<int> consume(int x) {
            auto [needs_sched, future] = send(producer_, &producer_actor::produce, x);
            if (needs_sched) {
                producer_obligations_.fetch_add(1, std::memory_order_acq_rel);
            }

            const int result = co_await std::move(future);

            completed_count_.fetch_add(1, std::memory_order_acq_rel);
            co_return result + 10;
        }

        using dispatch_traits = actor_zeta::dispatch_traits<&soak_consumer::consume>;

        behavior_t behavior(mailbox::message* msg) {
            switch (msg->command()) {
                case msg_id<soak_consumer, &soak_consumer::consume>:
                    co_await dispatch(this, &soak_consumer::consume, msg);
                    break;
                default:
                    break;
            }
        }

        int completed_count() const noexcept {
            return completed_count_.load(std::memory_order_acquire);
        }

        int take_producer_obligations() noexcept {
            return producer_obligations_.exchange(0, std::memory_order_acq_rel);
        }

        ~soak_consumer() = default;

    private:
        address_t producer_;
        std::atomic<int> completed_count_;
        std::atomic<int> producer_obligations_;
    };

    // `times` worker turns, returning the LAST verdict; on a parked actor, a bare enqueue().
    template<typename Actor>
    scheduler::resume_result resume_n(Actor* actor,
                                      int times,
                                      std::vector<std::string>& trace,
                                      const char* tag) {
        auto last = scheduler::resume_result::done;
        for (int i = 0; i < times; ++i) {
            last = actor->resume(/*max_throughput*/ 1).result;
            trace.emplace_back(std::string(tag) + "=" + to_string(last));
        }
        return last;
    }

    std::string render(const std::vector<std::string>& trace) {
        std::string out;
        for (const auto& entry : trace) {
            if (!out.empty()) {
                out += " -> ";
            }
            out += entry;
        }
        return out;
    }

} // namespace

TEST_CASE("lost-wakeup: a second sequential co_await must not strand the behavior") {
    auto* resource = std::pmr::get_default_resource();
    auto producer = spawn<producer_actor>(resource);
    auto consumer = spawn<chained_consumer>(resource, producer->address());

    std::vector<std::string> trace;

    auto [needs_sched, request] = send(consumer.get(), &chained_consumer::chain, 21);
    request.detach(); // progress is observed through the consumer's counters

    REQUIRE(needs_sched == true); // born parked: the constructor blocks the inbox

    // (1) Drive to the FIRST co_await; resume() stands in for enqueue + a worker.
    if (needs_sched) {
        resume_n(consumer.get(), 1, trace, "consumer");
    }
    REQUIRE(consumer->first_await_done() == false);
    REQUIRE(consumer->completed_count() == 0);

    // (2) Complete the FIRST future (flag-only), discharging the recorded obligation.
    trace.emplace_back(std::string("first_send_needs_sched=") +
                       std::to_string(consumer->first_send_needs_sched()));
    if (consumer->first_send_needs_sched() == 1) {
        resume_n(producer.get(), 2, trace, "producer");
    }

    // (3) Drain await #1, re-suspend on #2. THE load-bearing assertion: without
    // the is_busy() re-check this parks a pending await (`awaiting`, job dropped).
    const auto drain_verdict = resume_n(consumer.get(), 1, trace, "consumer");
    INFO("trace: " << render(trace));
    REQUIRE(drain_verdict == scheduler::resume_result::resume);

    REQUIRE(consumer->first_await_done() == true); // past #1, on #2: not a vacuous pass
    REQUIRE(consumer->completed_count() == 0);

    // (4) Complete the SECOND future (flag-only again).
    trace.emplace_back(std::string("second_send_needs_sched=") +
                       std::to_string(consumer->second_send_needs_sched()));
    if (consumer->second_send_needs_sched() == 1) {
        resume_n(producer.get(), 2, trace, "producer");
    }

    // (5) No message pushed on purpose -- the bare enqueue is the only wake-up once
    // parked -- and it must make progress whether or not the inbox is blocked.
    constexpr int kDrainCap = 64;
    for (int i = 0; i < kDrainCap && consumer->completed_count() == 0; ++i) {
        resume_n(consumer.get(), 1, trace, "consumer");
    }

    INFO("trace: " << render(trace));
    INFO("completed_count=" << consumer->completed_count()
                            << " result=" << consumer->result());
    REQUIRE(consumer->completed_count() == 1);
    REQUIRE(consumer->result() == 84); // 21 * 2 = 42, then 42 * 2 = 84
}

// What send() reports for a fresh actor and for one parked on an empty inbox, so a
// change to the inbox's initial state or enqueue_impl's mapping shows up here.
TEST_CASE("needs_sched contract: fresh actor and parked actor both report the obligation") {
    auto* resource = std::pmr::get_default_resource();
    auto producer = spawn<producer_actor>(resource);

    std::vector<std::string> trace;

    auto [needs_sched_fresh, fresh_future] = send(producer.get(), &producer_actor::produce, 1);
    fresh_future.detach();
    trace.emplace_back(std::string("fresh=") + (needs_sched_fresh ? "true" : "false"));

    if (needs_sched_fresh) {
        resume_n(producer.get(), 1, trace, "producer"); // discharge; it parks once the inbox runs dry
    }

    auto [needs_sched_parked, parked_future] = send(producer.get(), &producer_actor::produce, 2);
    parked_future.detach();
    trace.emplace_back(std::string("parked=") + (needs_sched_parked ? "true" : "false"));

    if (needs_sched_parked) {
        resume_n(producer.get(), 1, trace, "producer");
    }

    INFO("trace: " << render(trace));
    REQUIRE(needs_sched_fresh == true); // an unreported unscheduled target strands the message
    REQUIRE(needs_sched_parked == true);
}

// A behavior suspended on a pending co_await returns `resume` with zero messages
// handled indefinitely, so stop() cannot use "queue non-empty" as its exit condition.
TEST_CASE("scheduler_test_t::stop() terminates against a pending await") {
    auto* resource = std::pmr::get_default_resource();
    auto producer = spawn<producer_actor>(resource);
    auto consumer = spawn<soak_consumer>(resource, producer->address());

    // Declared last so it is destroyed first: its deque holds raw job pointers.
    test::scheduler_test_t sched(/*workers*/ 1, /*max_throughput*/ 1);

    auto [needs_sched, future] = send(consumer.get(), &soak_consumer::consume, 21);
    future.detach();
    if (needs_sched) {
        sched.enqueue(consumer.get());
    }

    // The producer is never scheduled, so the await stays pending: `resume`, nothing handled.
    for (int i = 0; i < 4; ++i) {
        sched.run_once();
    }
    REQUIRE(consumer->completed_count() == 0);

    sched.stop(); // the assertion IS termination: returning means stop() no longer hangs

    REQUIRE(consumer->completed_count() == 0);
}

// Soak through the real scheduler: the only coverage under contention. Tagged [stress]
// (`ctest -LE stress` excludes it). Pass/fail is decided by PROGRESS, not elapsed time:
// only kStallPolls polls with no progress fail; the absolute cap exists solely so a hang
// cannot wedge CI. The producer is enqueued on a RECORDED obligation, never a blind timer.
TEST_CASE("lost-wakeup: multi-thread soak, consumer co_awaits producer", "[stress]") {
    auto* resource = std::pmr::get_default_resource();

    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned num_workers = hw < 4u ? 4u : hw;
    auto scheduler = std::make_unique<scheduler::sharing_scheduler>(num_workers, 1);
    scheduler->start();

    auto producer = spawn<producer_actor>(resource); // outlives the senders; destroyed only after stop()
    auto consumer = spawn<soak_consumer>(resource, producer->address());

    constexpr int kSenderThreads = 4;
    constexpr int kRequestsPerThread = 2000;
    constexpr int kTotalRequests = kSenderThreads * kRequestsPerThread;

    std::atomic<int> submitted{0};
    std::atomic<bool> stop_pump{false};

    // A coroutine holds an address_t, not a scheduler: this thread discharges its obligations.
    std::thread producer_pump([&]() {
        while (!stop_pump.load(std::memory_order_acquire)) {
            if (consumer->take_producer_obligations() > 0) {
                scheduler->enqueue(producer.get());
            } else {
                // Safety net for an obligation recorded just after the counter was claimed.
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                scheduler->enqueue(producer.get());
            }
        }
    });

    std::vector<std::thread> senders;
    senders.reserve(kSenderThreads);
    for (int t = 0; t < kSenderThreads; ++t) {
        senders.emplace_back([&, t]() {
            for (int i = 0; i < kRequestsPerThread; ++i) {
                int x = t * kRequestsPerThread + i;
                auto [needs_sched, fut] = send(consumer.get(), &soak_consumer::consume, x);
                fut.detach(); // completion observed via consumer->completed_count()
                if (needs_sched) {
                    scheduler->enqueue(consumer.get());
                }
                submitted.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& s : senders) {
        s.join();
    }

    REQUIRE(submitted.load() == kTotalRequests);

    // kStallPolls of zero progress is a stall; kHangGuard only keeps CI from wedging.
    constexpr int kStallPolls = 400;
    constexpr auto kPollInterval = std::chrono::milliseconds(5);
    constexpr auto kHangGuard = std::chrono::minutes(3);

    const auto started = std::chrono::steady_clock::now();
    int last_seen = consumer->completed_count();
    int stall_polls = 0;
    bool hit_hang_guard = false;

    while (consumer->completed_count() < kTotalRequests) {
        std::this_thread::sleep_for(kPollInterval);

        const int now_seen = consumer->completed_count();
        if (now_seen != last_seen) {
            last_seen = now_seen;
            stall_polls = 0;
        } else if (++stall_polls >= kStallPolls) {
            break;
        }

        if (std::chrono::steady_clock::now() - started > kHangGuard) {
            hit_hang_guard = true;
            break;
        }
    }

    // Stop the scheduler BEFORE any actor dies; stop() drains in flight, so the count is final only after.
    stop_pump.store(true, std::memory_order_release);
    producer_pump.join();
    scheduler->stop();

    const int completed = consumer->completed_count();

    INFO("submitted=" << kTotalRequests
         << " completed=" << completed
         << " missing=" << (kTotalRequests - completed)
         << " num_workers=" << num_workers
         << " stalled_polls=" << stall_polls
         << " hit_hang_guard=" << (hit_hang_guard ? "yes" : "no"));

    // Any shortfall after a full drain is the lost wakeup: a consumer parked while pending.
    REQUIRE(completed == kTotalRequests);
}
