/// @file
/// Driving a unique_future from OUTSIDE an actor.
///
/// A caller that is not a cooperative_actor -- a CLI, a test, an Asio connection
/// handler, any foreign event loop -- still needs the value out of a
/// unique_future. There are exactly two supported ways.
///
///   ROUTE 1 -- POLLING (prefer this).
///     is_ready() / failed() / take_ready(). Works for EVERY future, including
///     the promise<T>-backed ones send() hands back. The poller never touches a
///     coroutine handle, so it can never pull an actor's frame onto its own
///     thread. This is what examples/asio does.
///
///   ROUTE 2 -- MANUAL DRAIN via coroutine_handle().
///     Only for a COROUTINE-BACKED future: one obtained by calling a method
///     coroutine directly instead of through send(). The caller reaches the
///     promise, reads the deepest awaited state, claims the continuation and
///     resumes it -- the drain block of cooperative_actor::resume_impl,
///     hand-rolled outside the actor.
///
///     Three things the caller owns and the framework cannot check:
///       * SERIALIZATION. This resumes an actor's frame outside the `running`
///         bit that cooperative_actor::try_acquire_running establishes. Never do
///         it while a scheduler may also be driving that actor.
///       * THE READINESS GATE. Claiming the continuation does not check whether
///         the awaited value exists. Resuming early takes from empty storage and
///         aborts. Gate on the value bit first, as awaited_is_ready() does below.
///       * THE FRAME. It is owned by the future, not by the handle: never
///         destroy() it. An empty handle is normal, not an error -- a
///         promise<T>-backed future has no producing coroutine, and a producer
///         that has already finished is parked at final_suspend and withheld.

#include <actor-zeta.hpp>
#include <actor-zeta/actor/dispatch.hpp>

#include <atomic>
#include <iostream>
#include <thread>

using namespace actor_zeta;

namespace {

    class producer_actor final : public basic_actor<producer_actor> {
    public:
        explicit producer_actor(std::pmr::memory_resource* resource)
            : basic_actor<producer_actor>(resource) {}

        unique_future<int> produce(int x) {
            std::cout << "  [producer] computing " << x << " * 2\n";
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
            , producer_needs_sched_(false) {}

        /// Public and callable directly on purpose: a direct call returns a
        /// coroutine-backed future, which is what route 2 needs. The same method
        /// reached through send() would return a promise<T>-backed future whose
        /// coroutine_handle() is empty.
        unique_future<int> consume(int x) {
            auto [needs_sched, future] = send(producer_, &producer_actor::produce, x);
            producer_needs_sched_.store(needs_sched, std::memory_order_release);

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

        bool producer_needs_sched() const noexcept {
            return producer_needs_sched_.load(std::memory_order_acquire);
        }

        ~consumer_actor() = default;

    private:
        address_t producer_;
        std::atomic<bool> producer_needs_sched_;
    };

    /// The readiness gate. Has the deepest future this coroutine is suspended on
    /// been completed by its producer? Completion is flag-only, so this is a
    /// plain atomic load -- nothing is resumed here.
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

    /// Claim the deepest awaited continuation and run it. Call ONLY when
    /// awaited_is_ready() is true.
    template<typename T>
    bool drain_awaited(const unique_future<T>& fut) {
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

} // namespace

int main() {
    auto* resource = std::pmr::get_default_resource();

    // ---------------------------------------------------------------- route 1
    std::cout << "ROUTE 1 - polling a send() future from a foreign thread\n";
    {
        auto producer = spawn<producer_actor>(resource);

        auto [needs_sched, future] = send(producer.get(), &producer_actor::produce, 21);
        std::cout << "  send() reported needs_sched=" << std::boolalpha << needs_sched << "\n";

        // A send() future is promise<T>-backed: there is no producing coroutine
        // to reach, so route 2 does not apply at all.
        std::cout << "  coroutine_handle() is empty: "
                  << !future.coroutine_handle() << "\n";

        // Bounded: if the producer is never driven -- needs_sched false, say --
        // an unbounded poller would make the join() below block forever.
        std::atomic<bool> seen{false};
        std::thread poller([&] {
            for (int i = 0; i < 1'000'000 && !future.is_ready(); ++i) {
                std::this_thread::yield();
            }
            seen.store(future.is_ready(), std::memory_order_release);
        });

        // The actor runs here, on this thread only. The poller never resumes it.
        if (needs_sched) {
            auto verdict = producer->resume(1);
            std::cout << "  resume() verdict handled, messages="
                      << verdict.messages_processed << "\n";
        }

        poller.join();
        std::cout << "  result = " << std::move(future).take_ready()
                  << " (observed by poller: " << seen.load() << ")\n\n";
    }

    // ---------------------------------------------------------------- route 2
    std::cout << "ROUTE 2 - manually draining a coroutine-backed future\n";
    {
        auto producer = spawn<producer_actor>(resource);
        auto consumer = spawn<consumer_actor>(resource, producer->address());

        // Direct call, NOT send(): this is what makes the future coroutine-backed.
        auto future = consumer->consume(21);

        auto handle = future.coroutine_handle();
        std::cout << "  handle is live: " << static_cast<bool>(handle)
                  << ", suspended: " << (handle && !handle.done()) << "\n";
        std::cout << "  awaited chain published: "
                  << (handle && handle.promise().awaited_flags_ != nullptr) << "\n";

        // The gate says no: the producer has not run, so the awaited value does
        // not exist yet. Draining here would resume past a co_await with nothing
        // to take and abort.
        std::cout << "  gate before producer runs: " << awaited_is_ready(future) << "\n";

        // Discharge the scheduling obligation the handler recorded. Completion is
        // flag-only -- it does NOT resume the consumer.
        if (consumer->producer_needs_sched()) {
            auto verdict = producer->resume(2);
            std::cout << "  producer driven, messages=" << verdict.messages_processed << "\n";
        }

        std::cout << "  gate after producer ran: " << awaited_is_ready(future) << "\n";

        // Now, and only now, the continuation is ours to claim. The gate is not
        // decoration: draining before the value exists resumes the consumer past
        // its co_await and takes from empty storage.
        const bool drained = awaited_is_ready(future) && drain_awaited(future);
        std::cout << "  drained: " << drained
                  << ", future ready: " << future.is_ready() << "\n";
        // `drained` can legitimately be false, and take_ready() only ASSERTS
        // readiness -- an assert examples do not have, because they ship Release.
        if (drained && future.is_ready() && !future.failed()) {
            std::cout << "  result = " << std::move(future).take_ready() << "\n";
        } else {
            std::cout << "  no value to take (drained=" << drained
                      << ", failed=" << future.failed() << ")\n";
        }
    }

    std::cout << "\nDone.\n";
    return 0;
}
