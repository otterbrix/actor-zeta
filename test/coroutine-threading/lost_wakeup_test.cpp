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

// ===========================================================================
// Regression guard for the lost wakeup in cooperative_actor::resume_impl.
//
// THE ROUTE (described structurally; do not pin it to line numbers, they rot)
//
//   A behavior suspended on co_await publishes its awaited chain, so the entry
//   path of resume_impl sees is_busy(). When the awaited future goes ready, the
//   Q6 block drains the continuation, the coroutine advances -- and if it issues
//   another request it re-suspends immediately, republishing a fresh awaited
//   chain.
//
//   The bug had two halves:
//
//     (a) the entry path did not re-check is_busy() after cont.resume(), so
//         control fell through to the empty-mailbox branch, try_block()
//         succeeded, and a behavior suspended on a PENDING await was parked
//         (verdict `awaiting`, keep_scheduled = false -> the worker drops the
//         job);
//     (b) the entry blocked-check sat ABOVE the Q6 block, so once an actor was
//         parked its ready continuation could never be drained: every later
//         resume died at the blocked-check before reaching Q6.
//
//   Future completion is flag-only -- release_promise sets promise_released, it
//   does not push to the awaiting actor's mailbox and does not enqueue it. So a
//   bare scheduler->enqueue() (a watchdog poke, a re-enqueue pump) is exactly
//   the wake-up that does NOT unblock the inbox, and (b) made it a no-op.
//
//   The fix is both halves: Q6 hoisted above the blocked-check, and an
//   unconditional is_busy() re-check after cont.resume().
//
// WHAT THIS FILE PINS
//
//   1. the deterministic route, driven by hand with resume(1) -- no threads, no
//      sleeps, no wall clock. The load-bearing assertion is on the VERDICT of
//      the drain step: it must be `resume`, not `awaiting`. Against the pre-fix
//      implementation that step parks the actor and the assertion fails.
//   2. the needs_sched contract that the route depends on.
//   3. a multi-threaded soak through the real scheduler ([stress]) -- the only
//      coverage of this route under contention. Its pass/fail is decided by
//      PROGRESS, never by a wall-clock budget: a slow or sanitized machine just
//      takes longer, it does not turn red.
// ===========================================================================

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

    // Plain cross-actor request/response worker, shared by every test below. Its
    // future is completed flag-only, so completing it never touches the
    // consumer's mailbox and never schedules the consumer.
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

    // ---- needs_sched: record-and-discharge -----------------------------------
    //
    // send() returns {needs_sched, future} and NOTHING inside the library
    // consumes that flag -- the SENDER owns the obligation to schedule the
    // target. A handler cannot discharge it itself: a coroutine has an address_t
    // and no scheduler. So the handler RECORDS the obligation and the driver
    // (a test loop below, or a real dispatcher in production) DISCHARGES it.
    // -------------------------------------------------------------------------

    // Consumer whose handler awaits the producer TWICE in sequence. The second
    // await is the one that gets stranded: it is published after the first
    // continuation has already been drained inside the entry path.
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

        unique_future<int> chain(int x) {
            auto [needs_sched_first, first_future] = send(producer_, &producer_actor::produce, x);
            first_send_needs_sched_.store(needs_sched_first ? 1 : 0, std::memory_order_release);

            const int first = co_await std::move(first_future);
            first_await_done_.store(true, std::memory_order_release);

            // Issued from INSIDE the entry-path cont.resume(): by the time control
            // returns to resume_impl the behavior is suspended again with a freshly
            // published awaited chain.
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

        // -1 = the send has not been issued yet, 0/1 = the reported obligation.
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
        std::atomic<int> first_send_needs_sched_;
        std::atomic<int> second_send_needs_sched_;
    };

    // Single-await consumer for the soak test. Records every scheduling
    // obligation its handler incurs so the driver can discharge them, instead of
    // the driver blind-poking the producer on a timer.
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

        // Claims the outstanding obligations, resetting the counter.
        int take_producer_obligations() noexcept {
            return producer_obligations_.exchange(0, std::memory_order_acq_rel);
        }

        ~soak_consumer() = default;

    private:
        address_t producer_;
        std::atomic<int> completed_count_;
        std::atomic<int> producer_obligations_;
    };

    // Resume an actor exactly `times` times, recording each verdict, and return
    // the LAST one. This is what a worker thread does per dequeued job -- and,
    // when the actor is parked, what a bare scheduler->enqueue() amounts to: a
    // resume with no message push.
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

    // A freshly spawned actor is already parked -- the constructor blocks the
    // inbox -- so the very first message must report the scheduling obligation.
    REQUIRE(needs_sched == true);

    // --- (1) Drive the consumer to its FIRST co_await. -----------------------
    // Discharging the obligation: resume() stands in for
    // `scheduler->enqueue(consumer)` plus a worker picking the job up. One resume
    // pops the request, starts the behavior, sends to the producer and suspends.
    if (needs_sched) {
        resume_n(consumer.get(), 1, trace, "consumer");
    }
    REQUIRE(consumer->first_await_done() == false);
    REQUIRE(consumer->completed_count() == 0);

    // --- (2) Complete the FIRST future (flag-only). --------------------------
    // The producer is resumed only if the handler's send actually reported the
    // obligation -- record-and-discharge, with the driver in the discharging role.
    trace.emplace_back(std::string("first_send_needs_sched=") +
                       std::to_string(consumer->first_send_needs_sched()));
    if (consumer->first_send_needs_sched() == 1) {
        resume_n(producer.get(), 2, trace, "producer");
    }

    // --- (3) The critical step: drain await #1, re-suspend on await #2. ------
    // THE load-bearing assertion of this file. Pre-fix, this resume drains the
    // first continuation, falls through to try_block() and PARKS a behavior that
    // is suspended on a pending await -> verdict `awaiting`, job dropped. With
    // the re-check in place the still-busy behavior stays schedulable ->
    // verdict `resume`.
    const auto drain_verdict = resume_n(consumer.get(), 1, trace, "consumer");
    INFO("trace: " << render(trace));
    REQUIRE(drain_verdict == scheduler::resume_result::resume);

    // Route guard: proves the test reached the interesting state instead of
    // passing vacuously. The behavior got past await #1 and is now suspended on
    // await #2, with its own mailbox empty.
    REQUIRE(consumer->first_await_done() == true);
    REQUIRE(consumer->completed_count() == 0);

    // --- (4) Complete the SECOND future (flag-only again). -------------------
    trace.emplace_back(std::string("second_send_needs_sched=") +
                       std::to_string(consumer->second_send_needs_sched()));
    if (consumer->second_send_needs_sched() == 1) {
        resume_n(producer.get(), 2, trace, "producer");
    }

    // --- (5) The payload invariant. ------------------------------------------
    // The consumer's awaited future is ready. Resuming the actor MUST make
    // progress -- whether or not its inbox happens to be blocked, because future
    // readiness is not delivered through the inbox. No message is pushed here on
    // purpose: this models a bare enqueue (watchdog poke / re-enqueue pump),
    // which is the only wake-up available once the actor is parked.
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

// ===========================================================================
// The needs_sched contract itself.
//
// send() returns {needs_sched, future} and NOTHING inside the library consumes
// that flag. This pins down what the framework reports in the two states that
// matter: a freshly spawned actor (never resumed) and an actor that parked
// itself on an empty inbox. Both are recorded, so a future change to the
// inbox's initial state or to enqueue_impl's mapping shows up here instead of
// silently altering who must schedule whom.
// ===========================================================================
TEST_CASE("needs_sched contract: fresh actor and parked actor both report the obligation") {
    auto* resource = std::pmr::get_default_resource();
    auto producer = spawn<producer_actor>(resource);

    std::vector<std::string> trace;

    // (a) First message ever sent to a freshly spawned, never-resumed actor.
    auto [needs_sched_fresh, fresh_future] = send(producer.get(), &producer_actor::produce, 1);
    fresh_future.detach();
    trace.emplace_back(std::string("fresh=") + (needs_sched_fresh ? "true" : "false"));

    // Discharge the obligation the way a scheduler would, and let the actor park
    // itself once its inbox runs dry.
    if (needs_sched_fresh) {
        resume_n(producer.get(), 1, trace, "producer");
    }

    // (b) Message sent to an actor that has parked itself (inbox blocked).
    auto [needs_sched_parked, parked_future] = send(producer.get(), &producer_actor::produce, 2);
    parked_future.detach();
    trace.emplace_back(std::string("parked=") + (needs_sched_parked ? "true" : "false"));

    if (needs_sched_parked) {
        resume_n(producer.get(), 1, trace, "producer");
    }

    INFO("trace: " << render(trace));
    // Recorded, not assumed: an unscheduled target must always be reported,
    // otherwise a sender honouring the contract would strand the message.
    REQUIRE(needs_sched_fresh == true);
    REQUIRE(needs_sched_parked == true);
}

// ===========================================================================
// scheduler_test_t must terminate against a legitimately spinning actor.
//
// A behavior suspended on a pending co_await returns `resume` with zero
// messages handled, for as long as the await stays pending. "queue non-empty"
// is therefore not a termination condition, and re-queueing such a job at the
// FRONT lets it monopolise the deque. Both used to be true, so stop() looped
// forever the moment anything in the queue was waiting on a future.
// ===========================================================================
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

    // Drive the consumer to its co_await. The producer is deliberately never
    // scheduled, so the awaited future stays pending forever and the consumer
    // keeps reporting `resume` with nothing handled.
    for (int i = 0; i < 4; ++i) {
        sched.run_once();
    }
    REQUIRE(consumer->completed_count() == 0);

    // The assertion IS termination: if this returns, stop() no longer hangs.
    sched.stop();

    REQUIRE(consumer->completed_count() == 0);
}

// ===========================================================================
// Multi-threaded soak through the real scheduler. Tagged [stress] so it can be
// excluded with `ctest -LE stress`.
//
// This is the only coverage of the route under contention, which is why it is
// kept rather than deleted. The previous incarnation decided pass/fail with a
// 15-second wall-clock budget, so a loaded CI runner or a sanitizer build
// reported a lost wakeup that never happened. Two changes fix that:
//
//   * pass/fail is decided by PROGRESS, not elapsed time. As long as completions
//     keep arriving the test keeps waiting, however slow the machine. Only a
//     genuine stall -- no progress at all across kStallPolls consecutive polls --
//     is a failure. The absolute cap exists solely so a hang cannot wedge CI,
//     and is reported distinctly from a stall.
//   * the producer is enqueued because the consumer RECORDED a scheduling
//     obligation, not on a blind timer, so the driver discharges the real
//     contract instead of masking it. The slow heartbeat is a safety net for
//     obligations racing the drain, and is documented as such.
// ===========================================================================
TEST_CASE("lost-wakeup: multi-thread soak, consumer co_awaits producer", "[stress]") {
    auto* resource = std::pmr::get_default_resource();

    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned num_workers = hw < 4u ? 4u : hw;
    auto scheduler = std::make_unique<scheduler::sharing_scheduler>(num_workers, 1);
    scheduler->start();

    // Long-lived actors: created before the senders, destroyed only after stop().
    auto producer = spawn<producer_actor>(resource);
    auto consumer = spawn<soak_consumer>(resource, producer->address());

    constexpr int kSenderThreads = 4;
    constexpr int kRequestsPerThread = 2000;
    constexpr int kTotalRequests = kSenderThreads * kRequestsPerThread;

    std::atomic<int> submitted{0};
    std::atomic<bool> stop_pump{false};

    // Discharges the consumer's recorded obligations. A coroutine cannot schedule
    // its target itself (it holds an address_t, not a scheduler), so this thread
    // stands in for the dispatcher that owns that job in production.
    std::thread producer_pump([&]() {
        while (!stop_pump.load(std::memory_order_acquire)) {
            if (consumer->take_producer_obligations() > 0) {
                scheduler->enqueue(producer.get());
            } else {
                // Safety net for an obligation recorded just after we claimed the
                // counter: keep the producer drainable without busy-spinning.
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

    // Progress-based wait. kStallPolls * kPollInterval is how long we insist on
    // seeing zero progress before calling it a stall; kHangGuard only prevents a
    // wedged CI job and is reported separately.
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

    // Tear down the pump, then stop the scheduler BEFORE destroying any actor.
    // stop() drains work still in flight, so the count is only final afterwards --
    // reading it before would report completions that did arrive as missing.
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

    // Any shortfall after a full drain is a parked-while-pending consumer that
    // the producer's flag-only completion could not wake: the lost wakeup.
    REQUIRE(completed == kTotalRequests);
}
