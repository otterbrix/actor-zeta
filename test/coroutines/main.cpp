#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/detail/future.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta/config.hpp>
#include <test/tooltestsuites/scheduler_test.hpp>

#include <atomic>

TEST_CASE("promise_type in unique_future<T>") {
    SECTION("promise_type exists for unique_future<int>") {
        using promise_type = actor_zeta::unique_future<int>::promise_type;
        REQUIRE(std::is_same_v<promise_type::value_type, int>);
    }

    SECTION("promise_type exists for unique_future<void>") {
        using promise_type = actor_zeta::unique_future<void>::promise_type;
        REQUIRE(std::is_same_v<promise_type::value_type, void>);
    }
}

// Methods are registered in dispatch_traits and reached through behavior(), so the
// tests below must go through send() rather than calling actor->method() directly.
class coroutine_test_actor final : public actor_zeta::basic_actor<coroutine_test_actor> {
public:
    explicit coroutine_test_actor(std::pmr::memory_resource* res)
        : actor_zeta::basic_actor<coroutine_test_actor>(res) {
    }

    actor_zeta::unique_future<int> coro_int() {
        co_return 42;
    }

    actor_zeta::unique_future<std::string> coro_string() {
        co_return std::string("hello");
    }

    actor_zeta::unique_future<void> coro_void() {
        co_return;
    }

    actor_zeta::unique_future<int> coro_storage() {
        co_return 100;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &coroutine_test_actor::coro_int,
        &coroutine_test_actor::coro_string,
        &coroutine_test_actor::coro_void,
        &coroutine_test_actor::coro_storage
    >;

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<coroutine_test_actor, &coroutine_test_actor::coro_int>:
                co_await dispatch(this, &coroutine_test_actor::coro_int, msg);
                break;
            case actor_zeta::msg_id<coroutine_test_actor, &coroutine_test_actor::coro_string>:
                co_await dispatch(this, &coroutine_test_actor::coro_string, msg);
                break;
            case actor_zeta::msg_id<coroutine_test_actor, &coroutine_test_actor::coro_void>:
                co_await dispatch(this, &coroutine_test_actor::coro_void, msg);
                break;
            case actor_zeta::msg_id<coroutine_test_actor, &coroutine_test_actor::coro_storage>:
                co_await dispatch(this, &coroutine_test_actor::coro_storage, msg);
                break;
        }
    }
};

TEST_CASE("simple coroutines with co_return") {
    auto* resource =std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<coroutine_test_actor>(resource);
    actor_zeta::test::scheduler_test_t sched(1, 100);

    SECTION("co_return int") {
        auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &coroutine_test_actor::coro_int
        );

        if (needs_sched) {
            sched.enqueue(actor.get());
        }
        sched.run();

        REQUIRE(future.valid());
        REQUIRE(future.is_ready());
        int result = std::move(future).take_ready();
        REQUIRE(result == 42);
    }

    SECTION("co_return string") {
        auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &coroutine_test_actor::coro_string
        );

        sched.enqueue(actor.get());
        sched.run();
        REQUIRE(future.valid());
        REQUIRE(future.is_ready());
        std::string result = std::move(future).take_ready();
        REQUIRE(result == "hello");
    }

    SECTION("co_return void") {
        auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &coroutine_test_actor::coro_void
        );

        sched.enqueue(actor.get());
        sched.run();
        REQUIRE(future.valid());
        REQUIRE(future.is_ready());
        std::move(future).take_ready();
    }
}

TEST_CASE("Coroutine futures") {
    auto* resource =std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<coroutine_test_actor>(resource);
    actor_zeta::test::scheduler_test_t sched(1, 100);

    SECTION("co_return creates valid future") {
        auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &coroutine_test_actor::coro_int
        );
        sched.enqueue(actor.get());
        sched.run();
        REQUIRE(future.valid());
        REQUIRE(future.is_ready());

        int result = std::move(future).take_ready();
        REQUIRE(result == 42);
    }

    SECTION("move constructor preserves future state") {
        auto [needs_sched, future1] = actor_zeta::send(
            actor.get(),
            &coroutine_test_actor::coro_string
        );
        sched.enqueue(actor.get());
        sched.run();
        REQUIRE(future1.valid());

        auto future2 = std::move(future1);
        REQUIRE(future2.valid());
        REQUIRE(future2.is_ready());

        std::string result = std::move(future2).take_ready();
        REQUIRE(result == "hello");
    }

    SECTION("cancel works on futures") {
        actor_zeta::promise<int> p(resource);
        auto future_state = p.get_future();

        REQUIRE(future_state.valid());
        REQUIRE_FALSE(future_state.failed());

        // Cancellation is expressed by setting an error on the promise
        p.error(std::make_error_code(std::errc::operation_canceled));
        REQUIRE(future_state.failed());
        REQUIRE(future_state.error() == std::make_error_code(std::errc::operation_canceled));
    }
}

class arithmetic_test_actor final : public actor_zeta::basic_actor<arithmetic_test_actor> {
public:
    explicit arithmetic_test_actor(std::pmr::memory_resource* res)
        : actor_zeta::basic_actor<arithmetic_test_actor>(res) {}

    actor_zeta::unique_future<int> coro_add(int a, int b) {
        co_return a + b;
    }

    actor_zeta::unique_future<std::string> coro_concat(std::string a, std::string b) {
        co_return a + b;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &arithmetic_test_actor::coro_add,
        &arithmetic_test_actor::coro_concat
    >;

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<arithmetic_test_actor, &arithmetic_test_actor::coro_add>:
                co_await actor_zeta::dispatch(this, &arithmetic_test_actor::coro_add, msg);
                break;
            case actor_zeta::msg_id<arithmetic_test_actor, &arithmetic_test_actor::coro_concat>:
                co_await actor_zeta::dispatch(this, &arithmetic_test_actor::coro_concat, msg);
                break;
        }
    }
};

TEST_CASE("coroutine methods with unique_future return type") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<arithmetic_test_actor>(resource);
    actor_zeta::test::scheduler_test_t sched(1, 100);

    SECTION("coro_add returns ready future") {
        auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &arithmetic_test_actor::coro_add, 10, 20
        );
        sched.enqueue(actor.get());
        sched.run();
        REQUIRE(future.valid());
        REQUIRE(future.is_ready());

        int result = std::move(future).take_ready();
        REQUIRE(result == 30);
    }

    SECTION("coro_concat returns ready future") {
        auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &arithmetic_test_actor::coro_concat,
            std::string("hello"), std::string(" world")
        );
        sched.enqueue(actor.get());
        sched.run();
        REQUIRE(future.valid());
        REQUIRE(future.is_ready());

        std::string result = std::move(future).take_ready();
        REQUIRE(result == "hello world");
    }

    SECTION("ready future - no waiting, instant get()") {
        auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &arithmetic_test_actor::coro_add, 5, 7
        );
        sched.enqueue(actor.get());
        sched.run();
        REQUIRE(future.is_ready());

        auto start = std::chrono::steady_clock::now();
        int result = std::move(future).take_ready();
        auto elapsed = std::chrono::steady_clock::now() - start;

        REQUIRE(result == 12);
        REQUIRE(elapsed < std::chrono::milliseconds(1));
    }
}

#include <actor-zeta.hpp>
#include <actor-zeta/send.hpp>

class future_test_actor final : public actor_zeta::basic_actor<future_test_actor> {
public:
    explicit future_test_actor(std::pmr::memory_resource* res)
        : actor_zeta::basic_actor<future_test_actor>(res) {
    }

    actor_zeta::unique_future<int> sync_add(int a, int b) {
        co_return a + b;
    }

    actor_zeta::unique_future<int> async_multiply(int a, int b) {
        co_return a * b;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &future_test_actor::sync_add,
        &future_test_actor::async_multiply
    >;

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<future_test_actor, &future_test_actor::sync_add>:
                co_await dispatch(this, &future_test_actor::sync_add, msg);
                break;
            case actor_zeta::msg_id<future_test_actor, &future_test_actor::async_multiply>:
                co_await dispatch(this, &future_test_actor::async_multiply, msg);
                break;
        }
    }
};

TEST_CASE("Handler integration - unique_future<T> return types") {
    auto* resource =std::pmr::get_default_resource();

    SECTION("sync method with ready future") {
        auto actor = actor_zeta::spawn<future_test_actor>(resource);
        actor_zeta::test::scheduler_test_t sched(1, 100);
        REQUIRE(actor != nullptr);

        auto [needs_sched, result] = actor_zeta::send(actor.get(), &future_test_actor::sync_add, 10, 20);

        REQUIRE(result.valid());

        sched.enqueue(actor.get());
        sched.run();
        REQUIRE(result.is_ready());
        int value = std::move(result).take_ready();
        REQUIRE(value == 30);
    }

    SECTION("async coroutine method") {
        auto actor = actor_zeta::spawn<future_test_actor>(resource);
        actor_zeta::test::scheduler_test_t sched(1, 100);
        REQUIRE(actor != nullptr);

        auto [needs_sched, result] = actor_zeta::send(actor.get(), &future_test_actor::async_multiply, 5, 7);

        REQUIRE(result.valid());

        sched.enqueue(actor.get());
        sched.run();
        REQUIRE(result.is_ready());
        int value = std::move(result).take_ready();
        REQUIRE(value == 35);
    }

    SECTION("multiple calls to sync method") {
        auto actor = actor_zeta::spawn<future_test_actor>(resource);
        actor_zeta::test::scheduler_test_t sched(1, 100);

        auto [ns1, r1] = actor_zeta::send(actor.get(), &future_test_actor::sync_add, 1, 2);
        auto [ns2, r2] = actor_zeta::send(actor.get(), &future_test_actor::sync_add, 3, 4);
        auto [ns3, r3] = actor_zeta::send(actor.get(), &future_test_actor::sync_add, 5, 6);

        sched.enqueue(actor.get());
        sched.run();
        REQUIRE(std::move(r1).take_ready() == 3);
        REQUIRE(std::move(r2).take_ready() == 7);
        REQUIRE(std::move(r3).take_ready() == 11);
    }

    SECTION("multiple calls to async method") {
        auto actor = actor_zeta::spawn<future_test_actor>(resource);
        actor_zeta::test::scheduler_test_t sched(1, 100);

        auto [ns1, r1] = actor_zeta::send(actor.get(), &future_test_actor::async_multiply, 2, 3);
        auto [ns2, r2] = actor_zeta::send(actor.get(), &future_test_actor::async_multiply, 4, 5);
        auto [ns3, r3] = actor_zeta::send(actor.get(), &future_test_actor::async_multiply, 6, 7);

        sched.enqueue(actor.get());
        sched.run();
        REQUIRE(std::move(r1).take_ready() == 6);
        REQUIRE(std::move(r2).take_ready() == 20);
        REQUIRE(std::move(r3).take_ready() == 42);
    }

    SECTION("mixed sync and async calls") {
        auto actor = actor_zeta::spawn<future_test_actor>(resource);
        actor_zeta::test::scheduler_test_t sched(1, 100);

        auto [ns1, sync_result] = actor_zeta::send(actor.get(), &future_test_actor::sync_add, 10, 5);
        auto [ns2, async_result] = actor_zeta::send(actor.get(), &future_test_actor::async_multiply, 3, 4);

        sched.enqueue(actor.get());
        sched.run();
        REQUIRE(std::move(sync_result).take_ready() == 15);
        REQUIRE(std::move(async_result).take_ready() == 12);
    }
}

// These sections only prove the lifecycle does not crash. The failure they guard
// against -- a coroutine frame that is never destroyed, taking its promise_type and
// locals with it -- is invisible without a sanitizer, so run this target under ASan
// or valgrind to actually detect it.

// An actor cannot await a message it posted to itself: while a behavior is suspended on a
// co_await, the loop keeps the turn and does not read the mailbox, so the message that would
// settle the future is never dispatched. A debug build stops the process at once
// (test/protocol-violations, self_await); in release the verdict stays `resume` forever.
#ifdef NDEBUG
class self_await_actor final : public actor_zeta::basic_actor<self_await_actor> {
public:
    explicit self_await_actor(std::pmr::memory_resource* res)
        : actor_zeta::basic_actor<self_await_actor>(res) {
    }

    actor_zeta::unique_future<int> inner() {
        inner_ran_.store(true, std::memory_order_release);
        co_return 7;
    }

    actor_zeta::unique_future<int> outer() {
        // Sent to ourselves: the mailbox is not blocked (we are running), so this
        // reports needs_sched == false and there is nothing to enqueue.
        auto [needs_sched, f] = actor_zeta::send(this, &self_await_actor::inner);
        actor_zeta::detail::ignore_unused(needs_sched);
        co_return co_await std::move(f);
    }

    bool inner_ran() const noexcept { return inner_ran_.load(std::memory_order_acquire); }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &self_await_actor::inner,
        &self_await_actor::outer
    >;

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<self_await_actor, &self_await_actor::inner>:
                co_await dispatch(this, &self_await_actor::inner, msg);
                break;
            case actor_zeta::msg_id<self_await_actor, &self_await_actor::outer>:
                co_await dispatch(this, &self_await_actor::outer, msg);
                break;
        }
    }

private:
    std::atomic<bool> inner_ran_{false};
};

TEST_CASE("Recursive coroutines are NOT SUPPORTED") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<self_await_actor>(resource);

    auto [needs_sched, future] = actor_zeta::send(actor.get(), &self_await_actor::outer);
    REQUIRE(needs_sched);

    // Bounded on purpose: the verdict never stops being `resume`, so an
    // unbounded pump would spin for good rather than fail.
    constexpr int kPumpCap = 64;
    int pumped = 0;
    for (int i = 0; i < kPumpCap && !future.is_ready(); ++i) {
        auto info = actor->resume(1);
        REQUIRE(info.result == actor_zeta::scheduler::resume_result::resume);
        ++pumped;
    }

    // The self-addressed message is still in the mailbox, undispatched.
    REQUIRE(pumped == kPumpCap);
    REQUIRE_FALSE(actor->inner_ran());
    REQUIRE_FALSE(future.is_ready());

    // outer() stays suspended on a co_await that can never settle. Destroying the actor
    // unwinds it: the chain's futures destroy their frames even mid-body, and the
    // caller's future settles with broken_pipe.
    future.detach();
}
#endif

TEST_CASE("coroutine cleanup does not crash") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<coroutine_test_actor>(resource);
    actor_zeta::test::scheduler_test_t sched(1, 100);

    SECTION("simple coroutine with co_return") {
        {
            auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &coroutine_test_actor::coro_int
            );
            sched.enqueue(actor.get());
            sched.run();
            REQUIRE(future.valid());
            REQUIRE(future.is_ready());
            int result = std::move(future).take_ready();
            REQUIRE(result == 42);
        }
        REQUIRE(true);
    }

    SECTION("multiple coroutines") {
        for (int i = 0; i < 100; ++i) {
            auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &coroutine_test_actor::coro_int
            );
            sched.enqueue(actor.get());
            sched.run();
            int result = std::move(future).take_ready();
            REQUIRE(result == 42);
        }
        REQUIRE(true);
    }

    SECTION("coroutine with string") {
        {
            auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &coroutine_test_actor::coro_string
            );
            sched.enqueue(actor.get());
            sched.run();
            REQUIRE(future.valid());
            std::string result = std::move(future).take_ready();
            REQUIRE(result == "hello");
        }
        REQUIRE(true);
    }

    SECTION("void coroutine") {
        {
            auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &coroutine_test_actor::coro_void
            );
            sched.enqueue(actor.get());
            sched.run();
            REQUIRE(future.valid());
            std::move(future).take_ready();
        }
        REQUIRE(true);
    }
}
// behavior() awaits a promise the test holds, so the test decides when the result is there.
class gated_behavior_actor final : public actor_zeta::basic_actor<gated_behavior_actor> {
public:
    explicit gated_behavior_actor(std::pmr::memory_resource* res)
        : actor_zeta::basic_actor<gated_behavior_actor>(res) {
    }

    actor_zeta::unique_future<void> noop() {
        co_return;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&gated_behavior_actor::noop>;

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message*) {
        co_await std::move(gate_);
    }

    actor_zeta::unique_future<void> gate_;
};

// The loop pulls a suspended behavior with take_awaited_continuation() alone: before the awaited
// result is there it must hand out nothing, or the continuation is gone and the behavior hangs.
TEST_CASE("take_awaited_continuation() hands the continuation out only once the result is ready") {
    std::pmr::unsynchronized_pool_resource resource;
    auto actor = actor_zeta::spawn<gated_behavior_actor>(&resource);
    actor_zeta::promise<void> gate(&resource);
    actor->gate_ = gate.get_future();

    actor_zeta::behavior_t behavior = actor->behavior(nullptr);
    REQUIRE(behavior.is_busy());
    REQUIRE_FALSE(behavior.take_awaited_continuation());

    gate.set_value();
    auto cont = behavior.take_awaited_continuation();
    REQUIRE(cont);
    cont.resume();
    REQUIRE_FALSE(behavior.is_busy());
}
