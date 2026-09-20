#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/actor/dispatch.hpp>

#include <atomic>
#include <thread>

// ===========================================================================
// Contract: driving a unique_future from OUTSIDE an actor.
//
// A caller that is not a cooperative_actor -- a test, an Asio connection
// handler, a foreign event loop -- still has to get a value out of a
// unique_future. The framework supports exactly two ways, and this file pins
// both, because both are load-bearing for downstream projects and neither had
// any coverage here.
//
//   1. POLLING: is_ready() / failed() / take_ready(). Works for every future,
//      including the promise<T>-backed ones send() returns. This is what
//      examples/asio does.
//
//   2. MANUAL DRAIN via coroutine_handle(): for a COROUTINE-BACKED future --
//      one obtained by calling a method coroutine directly rather than through
//      send() -- the caller may reach the promise, read the deepest awaited
//      state, claim the continuation and resume it. That is the Q6 block of
//      cooperative_actor::resume_impl, hand-rolled outside the actor.
//
// Route 2 is why unique_future<T>::coroutine_handle() is public. It looks like
// an internal leak and it is the one way to resume an actor's frame outside the
// `running`-bit critical section -- so the caller owns the serialization -- but
// it is a supported extension point, not an accident. Deleting it silently
// breaks every external driver built on it.
// ===========================================================================

using namespace actor_zeta;

namespace {

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

    class consumer_actor final : public basic_actor<consumer_actor> {
    public:
        consumer_actor(std::pmr::memory_resource* resource, address_t producer)
            : basic_actor<consumer_actor>(resource)
            , producer_(producer)
            , producer_needs_sched_(-1) {}

        // Deliberately public and callable directly: a direct call yields a
        // COROUTINE-BACKED future, which is the only kind route 2 applies to.
        // Going through send() would produce a promise<T>-backed future whose
        // coroutine_handle() is empty.
        unique_future<int> consume(int x) {
            auto [needs_sched, future] = send(producer_, &producer_actor::produce, x);
            producer_needs_sched_.store(needs_sched ? 1 : 0, std::memory_order_release);

            const int result = co_await std::move(future);
            co_return result + 10;
        }

        using dispatch_traits = actor_zeta::dispatch_traits<&consumer_actor::consume>;

        behavior_t behavior(mailbox::message* msg) {
            switch (msg->command()) {
                case msg_id<consumer_actor, &consumer_actor::consume>:
                    co_await dispatch(this, &consumer_actor::consume, msg);
                    break;
                default:
                    break;
            }
        }

        int producer_needs_sched() const noexcept {
            return producer_needs_sched_.load(std::memory_order_acquire);
        }

        ~consumer_actor() = default;

    private:
        address_t producer_;
        std::atomic<int> producer_needs_sched_;
    };

    // The shape an external driver hand-rolls to drain an awaited chain:
    // claim the deepest awaited continuation atomically and run it. Returns
    // false when nothing is suspended.
    template<typename T>
    bool resume_awaited(const unique_future<T>& fut) {
        auto handle = fut.coroutine_handle();
        if (!handle || handle.done()) {
            return false;
        }
        auto* cont_ptr = handle.promise().awaited_continuation_;
        if (!cont_ptr) {
            return false;
        }
        auto cont = cont_ptr->exchange(nullptr, std::memory_order_acq_rel);
        if (!cont) {
            return false;
        }
        cont.resume();
        return true;
    }

    // The same, wrapped in a bounded drive-until-ready loop:
    // poll, and whenever the deepest awaited future reports promise_released,
    // drain its continuation from this (non-actor) thread.
    template<typename T>
    bool awaited_is_ready(const unique_future<T>& fut) {
        auto handle = fut.coroutine_handle();
        if (!handle || handle.done()) {
            return false;
        }
        auto* flags = handle.promise().awaited_flags_;
        if (!flags) {
            return false;
        }
        // promise_released alone is NOT a readiness gate: a promise that dies
        // without a value sets it too -- a cancelled producer, an actor torn down
        // with queued work, a dropped promise (broken_pipe). Draining on that bit
        // resumes the consumer past its co_await with nothing to take, and
        // await_resume()'s assert(!has_error()) is compiled out under NDEBUG, so
        // the consumer reads unset storage and reports success.
        //
        // Require the value bit instead. An awaited future that completed with an
        // error is deliberately NOT drained here: await_resume() has no way to
        // report failure, so there is nothing safe to do with it from outside.
        const auto bits = flags->load(std::memory_order_acquire);
        return (bits & detail::state_flags::value_set) != 0
            && (bits & detail::state_flags::error_set) == 0;
    }

} // namespace

TEST_CASE("external drive: coroutine_handle() exposes the awaited chain of a method coroutine") {
    auto* resource = std::pmr::get_default_resource();
    auto producer = spawn<producer_actor>(resource);
    auto consumer = spawn<consumer_actor>(resource, producer->address());

    // Direct call, NOT send(): this is what makes the future coroutine-backed.
    auto fut = consumer->consume(21);

    // The coroutine ran inline up to its cross-actor co_await and is suspended
    // there, so the handle is live and the awaited chain is published.
    auto handle = fut.coroutine_handle();
    REQUIRE(handle);
    REQUIRE(!handle.done());
    REQUIRE(handle.promise().awaited_flags_ != nullptr);
    REQUIRE(handle.promise().awaited_continuation_ != nullptr);
    REQUIRE(!fut.is_ready());

    // Nothing is ready yet: the producer has not run.
    //
    // PRECONDITION, and the reason awaited_is_ready() exists: resume_awaited()
    // does NOT check readiness. The continuation is installed for as long as the
    // coroutine is suspended, so calling it here would resume the consumer past
    // a co_await whose value was never set -- await_resume then takes from empty
    // storage and aborts. The caller owns the gate; external drivers gate
    // on exactly this flag.
    REQUIRE(awaited_is_ready(fut) == false);

    // Discharge the obligation the handler recorded, so the producer completes
    // the future. Completion is flag-only: it does NOT resume the consumer.
    REQUIRE(consumer->producer_needs_sched() == 1);
    auto verdict = producer->resume(2).result;
    // `shutdown` is produced only by the scheduler's internal shutdown_helper,
    // never by cooperative_actor::resume(), so asserting against it proves
    // nothing. The verdict that matters is `resume`: it would mean the actor
    // still owes a scheduling turn that this hand-staged drive never gave it.
    REQUIRE(verdict != scheduler::resume_result::resume);

    // Now the deepest awaited state reports ready, and the continuation is ours
    // to claim -- this is the Q6 drain, performed from a non-actor thread.
    REQUIRE(awaited_is_ready(fut) == true);
    REQUIRE(resume_awaited(fut) == true);

    REQUIRE(fut.is_ready());
    REQUIRE(std::move(fut).take_ready() == 52); // 21 * 2 + 10
}

TEST_CASE("external drive: a send()-backed future has no coroutine handle") {
    auto* resource = std::pmr::get_default_resource();
    auto producer = spawn<producer_actor>(resource);

    auto [needs_sched, fut] = send(producer.get(), &producer_actor::produce, 21);
    REQUIRE(needs_sched == true);

    // send() hands back a promise<T>-backed future. There is no producing
    // coroutine to reach, so route 2 does not apply and route 1 is the only
    // option. An external driver must handle this case -- its
    // drive_until_ready guards on `if (handle && !handle.done())` for exactly
    // this reason.
    REQUIRE(!fut.coroutine_handle());
    REQUIRE(resume_awaited(fut) == false);

    auto verdict = producer->resume(1).result;
    REQUIRE(verdict != scheduler::resume_result::resume);

    REQUIRE(fut.is_ready());
    REQUIRE(std::move(fut).take_ready() == 42);
}

TEST_CASE("external drive: polling from a foreign thread never resumes an actor frame") {
    auto* resource = std::pmr::get_default_resource();
    auto producer = spawn<producer_actor>(resource);

    auto [needs_sched, fut] = send(producer.get(), &producer_actor::produce, 50);
    REQUIRE(needs_sched == true);

    // The actor is driven on one thread while a foreign thread only polls. This
    // is the supported integration shape (examples/asio and the
    // asio_future_bridge): the poller never touches a coroutine handle, so it
    // cannot pull an actor's frame onto its own thread.
    // The spin is bounded, and that bound IS the hang guard -- no stop flag. A
    // flag set from this thread would either race the poller (exit before it
    // notices readiness) or, if set after join(), never be readable at all.
    // If the future never completes, the poller returns on the bound and the
    // assertion below fails instead of the suite hanging.
    std::atomic<bool> observed{false};
    std::thread poller([&] {
        for (int i = 0; i < 10'000'000; ++i) {
            if (fut.is_ready()) {
                observed.store(true, std::memory_order_release);
                return;
            }
            std::this_thread::yield();
        }
    });

    auto verdict = producer->resume(1).result;
    REQUIRE(verdict != scheduler::resume_result::resume);

    poller.join();

    REQUIRE(observed.load(std::memory_order_acquire) == true);
    REQUIRE(std::move(fut).take_ready() == 100);
}

// ===========================================================================
// The handle's lifetime contract.
//
// The producing coroutine SELF-DESTROYS: final_awaiter::await_suspend calls
// self.destroy() (future.hpp, step 5) even on the path where the consumer's
// unique_future is still alive and still caching handle_. Nothing nulls that
// member, so coroutine_handle() would hand back a pointer into a freed frame
// and the next handle.done() is a heap-use-after-free -- ASan reports a READ
// of size 8 there.
//
// That is not theoretical: external drivers that hand-roll this carry a guard
// for exactly this case, and the
// drive_until_ready survives only because its loop gate happens to be the same
// bit that triggers the destruction.
//
// The contract that makes those driver loops safe: once the state reports
// ready, the handle is no longer handed out.
// ===========================================================================

TEST_CASE("external drive: coroutine_handle() is empty once a driven future completes") {
    auto* resource = std::pmr::get_default_resource();
    auto producer = spawn<producer_actor>(resource);
    auto consumer = spawn<consumer_actor>(resource, producer->address());

    auto fut = consumer->consume(21);
    REQUIRE(fut.coroutine_handle());        // suspended: the frame is alive and ours to read

    REQUIRE(consumer->producer_needs_sched() == 1);
    auto verdict = producer->resume(2).result;
    REQUIRE(verdict != scheduler::resume_result::resume);
    REQUIRE(resume_awaited(fut) == true);   // completes the consumer -> frame destroyed

    // A driver loop re-reads the gate right here. The frame is gone, so the
    // handle must be gone with it.
    REQUIRE(fut.is_ready());
    REQUIRE(!fut.coroutine_handle());

    // The same thing through the driver helper, which is what an external driver
    // actually runs: it re-reads the handle and calls done() on it.
    // Without the getter's guard that done() is a heap-use-after-free; with it
    // the helper simply reports "nothing to drain".
    REQUIRE(resume_awaited(fut) == false);

    REQUIRE(std::move(fut).take_ready() == 52);
}

TEST_CASE("external drive: a synchronous method coroutine never exposes its frame") {
    auto* resource = std::pmr::get_default_resource();
    auto producer = spawn<producer_actor>(resource);

    // produce() has no reachable suspension point and initial_suspend is
    // suspend_never, so it runs to completion -- and destroys its frame --
    // INSIDE this call, before the future is handed back. No loop required to
    // observe the dangling handle.
    auto fut = producer->produce(21);
    REQUIRE(fut.is_ready());
    REQUIRE(!fut.coroutine_handle());

    REQUIRE(std::move(fut).take_ready() == 42);
}
