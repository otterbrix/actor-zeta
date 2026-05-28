#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/detail/future.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta/config.hpp>

// ============================================================================
// Test 1: promise_type exists and can be used for co_return
// ============================================================================

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

// ============================================================================
// Test 2: Coroutine with co_return (actor member functions)
// ============================================================================

// Test actor with coroutine member functions
// ============================================================================
// IMPORTANT: This actor follows proper Actor Model principles:
// - Methods are registered in dispatch_traits
// - behavior() dispatches messages
// - Tests MUST use send() instead of direct calls (actor->method())
// ============================================================================
class coroutine_test_actor final : public actor_zeta::basic_actor<coroutine_test_actor> {
public:
    explicit coroutine_test_actor(std::pmr::memory_resource* res)
        : actor_zeta::basic_actor<coroutine_test_actor>(res) {
    }

    // Coroutine member functions - resource() extracted from 'this'
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

    SECTION("co_return int") {
        // FIXED: Use send() instead of direct call
        auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &coroutine_test_actor::coro_int
        );

        // Process message through actor
        actor->resume(100);

        REQUIRE(future.valid());
        REQUIRE(future.is_ready());
        int result = std::move(future).take_ready();
        REQUIRE(result == 42);
    }

    SECTION("co_return string") {
        // FIXED: Use send() instead of direct call
        auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &coroutine_test_actor::coro_string
        );

        // Process message through actor
        actor->resume(100);

        REQUIRE(future.valid());
        REQUIRE(future.is_ready());
        std::string result = std::move(future).take_ready();
        REQUIRE(result == "hello");
    }

    SECTION("co_return void") {
        // FIXED: Use send() instead of direct call
        auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &coroutine_test_actor::coro_void
        );

        // Process message through actor
        actor->resume(100);

        REQUIRE(future.valid());
        REQUIRE(future.is_ready());
        std::move(future).take_ready();  // Should not throw
    }
}

// ============================================================================
// Test 12: Coroutine futures (STATE mode)
// ============================================================================

TEST_CASE("Coroutine futures") {
    auto* resource =std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<coroutine_test_actor>(resource);

    SECTION("co_return creates valid future") {
        // Use send() instead of direct call
        auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &coroutine_test_actor::coro_int
        );
        actor->resume(100);

        REQUIRE(future.valid());
        REQUIRE(future.is_ready());

        int result = std::move(future).take_ready();
        REQUIRE(result == 42);
    }

    SECTION("move constructor preserves future state") {
        // Use send() instead of direct call
        auto [needs_sched, future1] = actor_zeta::send(
            actor.get(),
            &coroutine_test_actor::coro_string
        );
        actor->resume(100);

        REQUIRE(future1.valid());

        auto future2 = std::move(future1);
        REQUIRE(future2.valid());
        REQUIRE(future2.is_ready());

        std::string result = std::move(future2).take_ready();
        REQUIRE(result == "hello");
    }

    SECTION("cancel works on futures") {
        // Create future via promise (clean API)
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

// ============================================================================
// Test 13: All methods must be coroutines via actor (need resource())
// ============================================================================

// Test actor for arithmetic coroutine tests
class arithmetic_test_actor final : public actor_zeta::basic_actor<arithmetic_test_actor> {
public:
    explicit arithmetic_test_actor(std::pmr::memory_resource* res)
        : actor_zeta::basic_actor<arithmetic_test_actor>(res) {}

    // Coroutine method returning unique_future<int>
    actor_zeta::unique_future<int> coro_add(int a, int b) {
        co_return a + b;
    }

    // Coroutine method returning unique_future<std::string>
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

    SECTION("coro_add returns ready future") {
        // Use send() instead of direct call
        auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &arithmetic_test_actor::coro_add, 10, 20
        );
        actor->resume(100);

        REQUIRE(future.valid());
        REQUIRE(future.is_ready());

        int result = std::move(future).take_ready();
        REQUIRE(result == 30);
    }

    SECTION("coro_concat returns ready future") {
        // Use send() instead of direct call
        auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &arithmetic_test_actor::coro_concat,
            std::string("hello"), std::string(" world")
        );
        actor->resume(100);

        REQUIRE(future.valid());
        REQUIRE(future.is_ready());

        std::string result = std::move(future).take_ready();
        REQUIRE(result == "hello world");
    }

    SECTION("ready future - no waiting, instant get()") {
        // Use send() instead of direct call
        auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &arithmetic_test_actor::coro_add, 5, 7
        );
        actor->resume(100);

        REQUIRE(future.is_ready());

        // get() should return immediately without blocking
        auto start = std::chrono::steady_clock::now();
        int result = std::move(future).take_ready();
        auto elapsed = std::chrono::steady_clock::now() - start;

        REQUIRE(result == 12);
        // Should be near-instant (< 1ms)
        REQUIRE(elapsed < std::chrono::milliseconds(1));
    }
}

// ============================================================================
// Test 14: Handler integration - methods returning unique_future<T>
// ============================================================================

#include <actor-zeta.hpp>
#include <actor-zeta/send.hpp>

// Test actor with methods returning unique_future<T>
class future_test_actor final : public actor_zeta::basic_actor<future_test_actor> {
public:
    explicit future_test_actor(std::pmr::memory_resource* res)
        : actor_zeta::basic_actor<future_test_actor>(res) {
    }

    // All methods must be coroutines
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
        REQUIRE(actor != nullptr);

        // Send message and get result
        auto [needs_sched, result] = actor_zeta::send(actor.get(), &future_test_actor::sync_add, 10, 20);

        REQUIRE(result.valid());

        // Process the message
        actor->resume(100);

        REQUIRE(result.is_ready());
        int value = std::move(result).take_ready();
        REQUIRE(value == 30);
    }

    SECTION("async coroutine method") {
        auto actor = actor_zeta::spawn<future_test_actor>(resource);
        REQUIRE(actor != nullptr);

        // Send message and get result
        auto [needs_sched, result] = actor_zeta::send(actor.get(), &future_test_actor::async_multiply, 5, 7);

        REQUIRE(result.valid());

        // Process the message
        actor->resume(100);

        REQUIRE(result.is_ready());
        int value = std::move(result).take_ready();
        REQUIRE(value == 35);
    }

    SECTION("multiple calls to sync method") {
        auto actor = actor_zeta::spawn<future_test_actor>(resource);

        auto [ns1, r1] = actor_zeta::send(actor.get(), &future_test_actor::sync_add, 1, 2);
        auto [ns2, r2] = actor_zeta::send(actor.get(), &future_test_actor::sync_add, 3, 4);
        auto [ns3, r3] = actor_zeta::send(actor.get(), &future_test_actor::sync_add, 5, 6);

        // Process all messages
        actor->resume(100);

        REQUIRE(std::move(r1).take_ready() == 3);
        REQUIRE(std::move(r2).take_ready() == 7);
        REQUIRE(std::move(r3).take_ready() == 11);
    }

    SECTION("multiple calls to async method") {
        auto actor = actor_zeta::spawn<future_test_actor>(resource);

        auto [ns1, r1] = actor_zeta::send(actor.get(), &future_test_actor::async_multiply, 2, 3);
        auto [ns2, r2] = actor_zeta::send(actor.get(), &future_test_actor::async_multiply, 4, 5);
        auto [ns3, r3] = actor_zeta::send(actor.get(), &future_test_actor::async_multiply, 6, 7);

        // Process all messages
        actor->resume(100);

        REQUIRE(std::move(r1).take_ready() == 6);
        REQUIRE(std::move(r2).take_ready() == 20);
        REQUIRE(std::move(r3).take_ready() == 42);
    }

    SECTION("mixed sync and async calls") {
        auto actor = actor_zeta::spawn<future_test_actor>(resource);

        auto [ns1, sync_result] = actor_zeta::send(actor.get(), &future_test_actor::sync_add, 10, 5);
        auto [ns2, async_result] = actor_zeta::send(actor.get(), &future_test_actor::async_multiply, 3, 4);

        // Process all messages
        actor->resume(100);

        REQUIRE(std::move(sync_result).take_ready() == 15);
        REQUIRE(std::move(async_result).take_ready() == 12);
    }
}

// See test/coroutine-threading for current approach using co_await

// Test 17: Recursive coroutines are NOT SUPPORTED
// Recursive send(this, ...) from within a coroutine will deadlock
// because the actor is in "running" state and won't reschedule itself.
// This is a known architectural limitation.
//
// If you need recursion, use iterative algorithms instead.

// ============================================================================
// Test 18: Memory leak detection - coroutine cleanup
// ============================================================================
//
// NOTE: This test verifies that coroutines are cleaned up properly.
// Memory leak detection requires running with AddressSanitizer or Valgrind:
//
//   # With AddressSanitizer:
//   cmake -DCMAKE_CXX_FLAGS="-fsanitize=address" ...
//   ./tests_coroutines
//
//   # With Valgrind:
//   valgrind --leak-check=full ./tests_coroutines
//
// If set_coroutine() is missing/commented, you will see memory leaks for:
// - promise_type object (coroutine frame)
// - Coroutine local variables
//
// This test ensures basic coroutine lifecycle works without crashing.

TEST_CASE("coroutine cleanup does not crash") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<coroutine_test_actor>(resource);

    SECTION("simple coroutine with co_return") {
        // This test ensures basic coroutine lifecycle completes without crash
        // Memory leak detection requires ASAN or Valgrind
        {
            // FIXED: Use send() instead of direct call (Actor Model)
            auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &coroutine_test_actor::coro_int
            );
            actor->resume(100);
            REQUIRE(future.valid());
            REQUIRE(future.is_ready());
            int result = std::move(future).take_ready();
            REQUIRE(result == 42);
        }
        // If we reach here without crash, basic cleanup works
        // But memory leaks can only be detected with ASAN/Valgrind
        REQUIRE(true);
    }

    SECTION("multiple coroutines") {
        // Stress test - create/destroy many coroutines
        for (int i = 0; i < 100; ++i) {
            // FIXED: Use send() instead of direct call (Actor Model)
            auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &coroutine_test_actor::coro_int
            );
            actor->resume(100);
            int result = std::move(future).take_ready();
            REQUIRE(result == 42);
        }
        // No crash = basic cleanup works
        REQUIRE(true);
    }

    SECTION("coroutine with string") {
        {
            // FIXED: Use send() instead of direct call (Actor Model)
            auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &coroutine_test_actor::coro_string
            );
            actor->resume(100);
            REQUIRE(future.valid());
            std::string result = std::move(future).take_ready();
            REQUIRE(result == "hello");
        }
        REQUIRE(true);
    }

    SECTION("void coroutine") {
        {
            // FIXED: Use send() instead of direct call (Actor Model)
            auto [needs_sched, future] = actor_zeta::send(
            actor.get(),
            &coroutine_test_actor::coro_void
            );
            actor->resume(100);
            REQUIRE(future.valid());
            std::move(future).take_ready();  // Should not throw
        }
        REQUIRE(true);
    }
}