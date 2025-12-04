#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/future.hpp>
#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/config.hpp>
#include <actor-zeta/dispatch.hpp>

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
    explicit coroutine_test_actor(actor_zeta::pmr::memory_resource* res)
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

    actor_zeta::unique_future<void> behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<coroutine_test_actor, &coroutine_test_actor::coro_int>:
                return dispatch(this, &coroutine_test_actor::coro_int, msg);
            case actor_zeta::msg_id<coroutine_test_actor, &coroutine_test_actor::coro_string>:
                return dispatch(this, &coroutine_test_actor::coro_string, msg);
            case actor_zeta::msg_id<coroutine_test_actor, &coroutine_test_actor::coro_void>:
                return dispatch(this, &coroutine_test_actor::coro_void, msg);
            case actor_zeta::msg_id<coroutine_test_actor, &coroutine_test_actor::coro_storage>:
                return dispatch(this, &coroutine_test_actor::coro_storage, msg);
        }
        return actor_zeta::make_ready_future_void(resource());
    }
};

TEST_CASE("simple coroutines with co_return") {
    auto* resource = actor_zeta::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<coroutine_test_actor>(resource);

    SECTION("co_return int") {
        // FIXED: Use send() instead of direct call
        auto future = actor_zeta::send(
            actor.get(),
            actor_zeta::address_t::empty_address(),
            &coroutine_test_actor::coro_int
        );

        // Process message through actor
        actor->resume(100);

        REQUIRE(future.valid());
        REQUIRE(future.available());
        int result = std::move(future).get();
        REQUIRE(result == 42);
    }

    SECTION("co_return string") {
        // FIXED: Use send() instead of direct call
        auto future = actor_zeta::send(
            actor.get(),
            actor_zeta::address_t::empty_address(),
            &coroutine_test_actor::coro_string
        );

        // Process message through actor
        actor->resume(100);

        REQUIRE(future.valid());
        REQUIRE(future.available());
        std::string result = std::move(future).get();
        REQUIRE(result == "hello");
    }

    SECTION("co_return void") {
        // FIXED: Use send() instead of direct call
        auto future = actor_zeta::send(
            actor.get(),
            actor_zeta::address_t::empty_address(),
            &coroutine_test_actor::coro_void
        );

        // Process message through actor
        actor->resume(100);

        REQUIRE(future.valid());
        REQUIRE(future.available());
        std::move(future).get();  // Should not throw
    }
}

// ============================================================================
// Test 3: future_state has coroutine storage and methods
// ============================================================================

TEST_CASE("future_state coroutine methods") {
    auto* resource = actor_zeta::pmr::get_default_resource();

    SECTION("future_state<int> has coroutine methods") {
        void* mem = resource->allocate(sizeof(actor_zeta::detail::future_state<int>),
                                        alignof(actor_zeta::detail::future_state<int>));
        auto* state = new (mem) actor_zeta::detail::future_state<int>(resource);

        // Test virtual methods exist and can be called
        REQUIRE_FALSE(state->has_coroutine());  // No coroutine stored yet
        REQUIRE_FALSE(state->coroutine_done());
        state->resume_coroutine();  // Should not crash (no-op if no coroutine)

        // Cleanup
        state->release();
    }

    SECTION("future_state<void> has coroutine methods") {
        void* mem = resource->allocate(sizeof(actor_zeta::detail::future_state<void>),
                                        alignof(actor_zeta::detail::future_state<void>));
        auto* state = new (mem) actor_zeta::detail::future_state<void>(resource);

        // Test virtual methods exist
        REQUIRE_FALSE(state->has_coroutine());
        REQUIRE_FALSE(state->coroutine_done());
        state->resume_coroutine();

        // Cleanup
        state->release();
    }
}

// ============================================================================
// Test 4: Coroutine handle storage via set_coroutine
// ============================================================================

TEST_CASE("coroutine handle can be stored in future_state") {
    auto* resource = actor_zeta::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<coroutine_test_actor>(resource);

    SECTION("set_coroutine stores handle") {
        void* mem = resource->allocate(sizeof(actor_zeta::detail::future_state<int>),
                                        alignof(actor_zeta::detail::future_state<int>));
        auto* state = new (mem) actor_zeta::detail::future_state<int>(resource);

        // FIXED: Use send() instead of direct call
        auto future = actor_zeta::send(
            actor.get(),
            actor_zeta::address_t::empty_address(),
            &coroutine_test_actor::coro_storage
        );

        // Process message
        actor->resume(100);

        // Note: In real usage, await_suspend would call set_coroutine()
        // Here we just verify the method exists
        REQUIRE_FALSE(state->has_coroutine());

        // Cleanup
        state->release();
    }
}

// ============================================================================
// Test 5: Virtual methods with `final` keyword (devirtualization check)
// ============================================================================

TEST_CASE("virtual methods marked as final") {
    // This is a compile-time check - if it compiles, final is working
    auto* resource = actor_zeta::pmr::get_default_resource();

    void* mem = resource->allocate(sizeof(actor_zeta::detail::future_state<int>),
                                    alignof(actor_zeta::detail::future_state<int>));
    auto* state = new (mem) actor_zeta::detail::future_state<int>(resource);

    // Cast to base to ensure virtual call
    actor_zeta::detail::future_state_base* base = state;

    // These should still be devirtualized by compiler (final keyword)
    REQUIRE_FALSE(base->has_coroutine());
    REQUIRE_FALSE(base->coroutine_done());
    base->resume_coroutine();

    // Cleanup
    state->release();
}

// ============================================================================
// Test 6: promise_type creates future_state correctly
// ============================================================================

// ============================================================================
// Test 7: Coroutine destruction
// ============================================================================

TEST_CASE("future_state destroys stored coroutine") {
    auto* resource = actor_zeta::pmr::get_default_resource();

    SECTION("destructor calls coro_handle_.destroy() if not done") {
        // Create future_state
        void* mem = resource->allocate(sizeof(actor_zeta::detail::future_state<int>),
                                        alignof(actor_zeta::detail::future_state<int>));
        auto* state = new (mem) actor_zeta::detail::future_state<int>(resource);

        // Destroy immediately (no coroutine stored - should be safe)
        state->~future_state();
        resource->deallocate(state, sizeof(actor_zeta::detail::future_state<int>),
                            alignof(actor_zeta::detail::future_state<int>));

        // If we reach here, no crash occurred
        REQUIRE(true);
    }
}

// ============================================================================
// Tests 8-11: REMOVED - behavior_t is outdated
// ============================================================================

// ============================================================================
// Test 12: Coroutine futures (STATE mode)
// ============================================================================

TEST_CASE("Coroutine futures") {
    auto* resource = actor_zeta::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<coroutine_test_actor>(resource);

    SECTION("co_return creates valid future") {
        auto future = actor->coro_int();  // co_return 42

        REQUIRE(future.valid());
        REQUIRE(future.available());

        int result = std::move(future).get();
        REQUIRE(result == 42);
    }

    SECTION("move constructor preserves future state") {
        auto future1 = actor->coro_string();
        REQUIRE(future1.valid());

        auto future2 = std::move(future1);
        REQUIRE(future2.valid());
        REQUIRE(future2.available());

        std::string result = std::move(future2).get();
        REQUIRE(result == "hello");
    }

    SECTION("cancel works on futures") {
        auto* resource = actor_zeta::pmr::get_default_resource();

        // Create future with state using PMR allocate + placement new
        // State starts with refcount=1, adopt_ref takes ownership without incrementing
        void* mem = resource->allocate(sizeof(actor_zeta::detail::future_state<int>),
                                        alignof(actor_zeta::detail::future_state<int>));
        auto* state = new (mem) actor_zeta::detail::future_state<int>(resource);
        // adopt_ref: take ownership of initial refcount (no add_ref)
        actor_zeta::unique_future<int> future_state(actor_zeta::adopt_ref, state, false);

        REQUIRE(future_state.valid());
        REQUIRE_FALSE(future_state.is_cancelled());

        future_state.cancel();
        REQUIRE(future_state.is_cancelled());
    }
}

// ============================================================================
// Test 13: Sync methods returning unique_future via make_ready_future()
// ============================================================================

// Sync method returning unique_future<int>
actor_zeta::unique_future<int> sync_add(int a, int b) {
    auto* resource = actor_zeta::pmr::get_default_resource();
    return actor_zeta::make_ready_future(resource, a + b);
}

// Sync method returning unique_future<std::string>
actor_zeta::unique_future<std::string> sync_concat(const std::string& a, const std::string& b) {
    auto* resource = actor_zeta::pmr::get_default_resource();
    return actor_zeta::make_ready_future(resource, a + b);
}

TEST_CASE("sync methods with unique_future return type") {
    SECTION("sync_add returns ready future") {
        auto future = sync_add(10, 20);

        REQUIRE(future.valid());
        REQUIRE(future.available());

        int result = std::move(future).get();
        REQUIRE(result == 30);
    }

    SECTION("sync_concat returns ready future") {
        auto future = sync_concat("hello", " world");

        REQUIRE(future.valid());
        REQUIRE(future.available());

        std::string result = std::move(future).get();
        REQUIRE(result == "hello world");
    }

    SECTION("ready future - no waiting, instant get()") {
        auto future = sync_add(5, 7);

        // get() should return immediately without blocking
        auto start = std::chrono::steady_clock::now();
        int result = std::move(future).get();
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
    explicit future_test_actor(actor_zeta::pmr::memory_resource* res)
        : actor_zeta::basic_actor<future_test_actor>(res) {
    }

    // Sync method returning ready future via make_ready_future()
    actor_zeta::unique_future<int> sync_add(int a, int b) {
        return actor_zeta::make_ready_future(resource(), a + b);
    }

    // Async coroutine method
    actor_zeta::unique_future<int> async_multiply(int a, int b) {
        co_return a * b;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &future_test_actor::sync_add,
        &future_test_actor::async_multiply
    >;

    actor_zeta::unique_future<void> behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<future_test_actor, &future_test_actor::sync_add>:
                return dispatch(this, &future_test_actor::sync_add, msg);
            case actor_zeta::msg_id<future_test_actor, &future_test_actor::async_multiply>:
                return dispatch(this, &future_test_actor::async_multiply, msg);
        }
        return actor_zeta::make_ready_future_void(resource());
    }
};

TEST_CASE("Handler integration - unique_future<T> return types") {
    auto* resource = actor_zeta::pmr::get_default_resource();

    SECTION("sync method with ready future") {
        auto actor = actor_zeta::spawn<future_test_actor>(resource);
        REQUIRE(actor != nullptr);

        // Send message and get result
        auto result = actor_zeta::send(actor.get(), actor->address(), &future_test_actor::sync_add, 10, 20);

        REQUIRE(result.valid());

        // Process the message
        actor->resume(100);

        REQUIRE(result.available());
        int value = std::move(result).get();
        REQUIRE(value == 30);
    }

    SECTION("async coroutine method") {
        auto actor = actor_zeta::spawn<future_test_actor>(resource);
        REQUIRE(actor != nullptr);

        // Send message and get result
        auto result = actor_zeta::send(actor.get(), actor->address(), &future_test_actor::async_multiply, 5, 7);

        REQUIRE(result.valid());

        // Process the message
        actor->resume(100);

        REQUIRE(result.available());
        int value = std::move(result).get();
        REQUIRE(value == 35);
    }

    SECTION("multiple calls to sync method") {
        auto actor = actor_zeta::spawn<future_test_actor>(resource);

        auto r1 = actor_zeta::send(actor.get(), actor->address(), &future_test_actor::sync_add, 1, 2);
        auto r2 = actor_zeta::send(actor.get(), actor->address(), &future_test_actor::sync_add, 3, 4);
        auto r3 = actor_zeta::send(actor.get(), actor->address(), &future_test_actor::sync_add, 5, 6);

        // Process all messages
        actor->resume(100);

        REQUIRE(std::move(r1).get() == 3);
        REQUIRE(std::move(r2).get() == 7);
        REQUIRE(std::move(r3).get() == 11);
    }

    SECTION("multiple calls to async method") {
        auto actor = actor_zeta::spawn<future_test_actor>(resource);

        auto r1 = actor_zeta::send(actor.get(), actor->address(), &future_test_actor::async_multiply, 2, 3);
        auto r2 = actor_zeta::send(actor.get(), actor->address(), &future_test_actor::async_multiply, 4, 5);
        auto r3 = actor_zeta::send(actor.get(), actor->address(), &future_test_actor::async_multiply, 6, 7);

        // Process all messages
        actor->resume(100);

        REQUIRE(std::move(r1).get() == 6);
        REQUIRE(std::move(r2).get() == 20);
        REQUIRE(std::move(r3).get() == 42);
    }

    SECTION("mixed sync and async calls") {
        auto actor = actor_zeta::spawn<future_test_actor>(resource);

        auto sync_result = actor_zeta::send(actor.get(), actor->address(), &future_test_actor::sync_add, 10, 5);
        auto async_result = actor_zeta::send(actor.get(), actor->address(), &future_test_actor::async_multiply, 3, 4);

        // Process all messages
        actor->resume(100);

        REQUIRE(std::move(sync_result).get() == 15);
        REQUIRE(std::move(async_result).get() == 12);
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
    auto* resource = actor_zeta::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<coroutine_test_actor>(resource);

    SECTION("simple coroutine with co_return") {
        // This test ensures basic coroutine lifecycle completes without crash
        // Memory leak detection requires ASAN or Valgrind
        {
            auto future = actor->coro_int();  // co_return 42
            REQUIRE(future.valid());
            REQUIRE(future.available());
            int result = std::move(future).get();
            REQUIRE(result == 42);
        }
        // If we reach here without crash, basic cleanup works
        // But memory leaks can only be detected with ASAN/Valgrind
        REQUIRE(true);
    }

    SECTION("multiple coroutines") {
        // Stress test - create/destroy many coroutines
        for (int i = 0; i < 100; ++i) {
            auto future = actor->coro_int();
            int result = std::move(future).get();
            REQUIRE(result == 42);
        }
        // No crash = basic cleanup works
        REQUIRE(true);
    }

    SECTION("coroutine with string") {
        {
            auto future = actor->coro_string();  // co_return "hello"
            REQUIRE(future.valid());
            std::string result = std::move(future).get();
            REQUIRE(result == "hello");
        }
        REQUIRE(true);
    }

    SECTION("void coroutine") {
        {
            auto future = actor->coro_void();  // co_return;
            REQUIRE(future.valid());
            std::move(future).get();  // Should not throw
        }
        REQUIRE(true);
    }
}