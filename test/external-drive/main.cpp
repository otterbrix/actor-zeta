#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/actor/dispatch.hpp>

#include <atomic>
#include <thread>

// Driving a unique_future from OUTSIDE an actor. Two supported routes, pinned here
// because downstream drivers depend on them: (1) POLLING -- is_ready()/failed()/
// take_ready(), works for every future; (2) MANUAL DRAIN via coroutine_handle() --
// COROUTINE-BACKED futures only (a method called directly, not through send()): read
// the deepest awaited state, claim the continuation, resume it. Route 2 is why
// coroutine_handle() is public: the one way to resume a frame outside `running`.

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

        // Called directly on purpose: only a direct call yields a COROUTINE-BACKED future.
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

    // The drain a driver hand-rolls: claim the deepest awaited continuation and run it.
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
        // promise_released is NOT a gate: a promise dying without a value sets it too.
        // Require the value bit; an error completion is never drained from outside.
        const auto bits = flags->load(std::memory_order_acquire);
        return (bits & detail::state_flags::value_set) != 0
            && (bits & detail::state_flags::error_set) == 0;
    }

} // namespace

TEST_CASE("external drive: coroutine_handle() exposes the awaited chain of a method coroutine") {
    auto* resource = std::pmr::get_default_resource();
    auto producer = spawn<producer_actor>(resource);
    auto consumer = spawn<consumer_actor>(resource, producer->address());

    auto fut = consumer->consume(21); // direct call, NOT send(): coroutine-backed

    auto handle = fut.coroutine_handle();
    REQUIRE(handle); // suspended at its cross-actor co_await
    REQUIRE(!handle.done());
    REQUIRE(handle.promise().awaited_flags_ != nullptr);
    REQUIRE(handle.promise().awaited_continuation_ != nullptr);
    REQUIRE(!fut.is_ready());

    // resume_awaited() does NOT gate on readiness; calling it now would resume past an unset value.
    REQUIRE(awaited_is_ready(fut) == false);

    REQUIRE(consumer->producer_needs_sched() == 1); // discharge it; completion is flag-only, no resume
    auto verdict = producer->resume(2).result;
    // != resume, not == done: `shutdown` is scheduler-only, and `resume` would mean an owed turn.
    REQUIRE(verdict != scheduler::resume_result::resume);

    REQUIRE(awaited_is_ready(fut) == true);
    REQUIRE(resume_awaited(fut) == true); // the entry-path drain, from a non-actor thread

    REQUIRE(fut.is_ready());
    REQUIRE(std::move(fut).take_ready() == 52); // 21 * 2 + 10
}

TEST_CASE("external drive: a send()-backed future has no coroutine handle") {
    auto* resource = std::pmr::get_default_resource();
    auto producer = spawn<producer_actor>(resource);

    auto [needs_sched, fut] = send(producer.get(), &producer_actor::produce, 21);
    REQUIRE(needs_sched == true);

    // promise<T>-backed: no producing coroutine, so route 2 does not apply.
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

    // The examples/asio shape: a foreign thread only polls and never touches a handle.
    // The spin bound IS the hang guard; a stop flag would race the poller or be set too late.
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

// Once the state is ready coroutine_handle() returns empty: the finished producer parks
// at final_suspend, release() reclaims the frame, and a re-reading driver loop never touches it.

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

    // A driver loop re-reads the gate right here; the handle must be withheld.
    REQUIRE(fut.is_ready());
    REQUIRE(!fut.coroutine_handle());

    REQUIRE(resume_awaited(fut) == false); // re-reads the handle, would call done(): must see nothing

    REQUIRE(std::move(fut).take_ready() == 52);
}

TEST_CASE("external drive: a synchronous method coroutine never exposes its frame") {
    auto* resource = std::pmr::get_default_resource();
    auto producer = spawn<producer_actor>(resource);

    // No suspension point and initial_suspend is suspend_never: it finishes INSIDE this call.
    auto fut = producer->produce(21);
    REQUIRE(fut.is_ready());
    REQUIRE(!fut.coroutine_handle());

    REQUIRE(std::move(fut).take_ready() == 42);
}
