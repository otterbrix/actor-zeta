/// @file
/// Driving a unique_future from OUTSIDE an actor. Two supported routes.
///   ROUTE 1 -- POLLING (prefer): is_ready()/failed()/take_ready(). Works for every
///     future; the poller never touches a handle, so it cannot pull a frame onto its thread.
///   ROUTE 2 -- MANUAL DRAIN via coroutine_handle(): COROUTINE-BACKED futures only (a
///     method called directly, not through send()). The caller owns what the framework
///     cannot check: SERIALIZATION (never while a scheduler may drive the actor), the
///     READINESS GATE (claim only after the value bit, else it takes from empty storage),
///     and the FRAME (owned by the future: never destroy() the handle; empty is normal).

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

        /// Called directly on purpose: only a direct call yields a coroutine-backed future.
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
        // promise_released is NOT a gate: a promise dying without a value sets it too, and
        // await_resume() refuses a valueless extraction by aborting. Require the value bit; never drain an error.
        const auto bits = flags->load(std::memory_order_acquire);
        return (bits & detail::state_flags::value_set) != 0
            && (bits & detail::state_flags::error_set) == 0;
    }

    /// Claim the deepest awaited continuation and run it. ONLY after awaited_is_ready().
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

    std::cout << "ROUTE 1 - polling a send() future from a foreign thread\n";
    {
        auto producer = spawn<producer_actor>(resource);

        auto [needs_sched, future] = send(producer.get(), &producer_actor::produce, 21);
        std::cout << "  send() reported needs_sched=" << std::boolalpha << needs_sched << "\n";

        // promise<T>-backed: no producing coroutine, so route 2 does not apply.
        std::cout << "  coroutine_handle() is empty: "
                  << !future.coroutine_handle() << "\n";

        std::atomic<bool> seen{false};
        std::thread poller([&] {
            for (int i = 0; i < 1'000'000 && !future.is_ready(); ++i) { // bounded: an undriven producer must not block join()
                std::this_thread::yield();
            }
            seen.store(future.is_ready(), std::memory_order_release);
        });

        if (needs_sched) {
            auto verdict = producer->resume(1); // the actor runs on this thread only
            std::cout << "  resume() verdict handled, messages="
                      << verdict.messages_processed << "\n";
        }

        poller.join();
        std::cout << "  result = " << std::move(future).take_ready()
                  << " (observed by poller: " << seen.load() << ")\n\n";
    }

    std::cout << "ROUTE 2 - manually draining a coroutine-backed future\n";
    {
        auto producer = spawn<producer_actor>(resource);
        auto consumer = spawn<consumer_actor>(resource, producer->address());

        auto future = consumer->consume(21); // direct call, NOT send(): coroutine-backed

        auto handle = future.coroutine_handle();
        std::cout << "  handle is live: " << static_cast<bool>(handle)
                  << ", suspended: " << (handle && !handle.done()) << "\n";
        std::cout << "  awaited chain published: "
                  << (handle && handle.promise().awaited_flags_ != nullptr) << "\n";

        // The gate says no: the producer has not run. Draining here would abort.
        std::cout << "  gate before producer runs: " << awaited_is_ready(future) << "\n";

        // Discharge the recorded obligation. Completion is flag-only: the consumer is NOT resumed.
        if (consumer->producer_needs_sched()) {
            auto verdict = producer->resume(2);
            std::cout << "  producer driven, messages=" << verdict.messages_processed << "\n";
        }

        std::cout << "  gate after producer ran: " << awaited_is_ready(future) << "\n";

        const bool drained = awaited_is_ready(future) && drain_awaited(future); // the gate is not decoration
        std::cout << "  drained: " << drained
                  << ", future ready: " << future.is_ready() << "\n";
        // take_ready() aborts on a valueless future in every build: gate on failed().
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
