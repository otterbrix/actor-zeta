#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <iostream>
#include <memory_resource>
#include <utility>

// ============================================================================
// Test: CRTP static polymorphism for enqueue_impl without virtual methods
// ============================================================================

namespace {

    // Simulated enqueue result
    enum class enqueue_result { success, queue_closed };

    // Track which method was called
    struct call_tracker {
        static int base_calls;
        static int derived_calls;
        static void reset() { base_calls = 0; derived_calls = 0; }
    };
    int call_tracker::base_calls = 0;
    int call_tracker::derived_calls = 0;

    // ========================================================================
    // Base class (like actor_mixin) - has enqueue_impl for sync processing
    // ========================================================================
    template<typename Derived>
    class base_mixin {
    public:
        std::pmr::memory_resource* resource() const {
            return std::pmr::get_default_resource();
        }

        // Base enqueue_impl - sync processing
        std::pair<enqueue_result, bool> enqueue_impl(int msg) {
            call_tracker::base_calls++;
            // Sync: call behavior directly via CRTP
            static_cast<Derived*>(this)->behavior(msg);
            return {enqueue_result::success, false};
        }

    protected:
        base_mixin() = default;
        ~base_mixin() = default;
    };

    // ========================================================================
    // Derived class with mailbox (like cooperative_actor) - HIDES base method
    // ========================================================================
    template<typename Actor>
    class actor_with_mailbox : public base_mixin<Actor> {
    public:
        // Derived enqueue_impl - async processing (hides base class method)
        std::pair<enqueue_result, bool> enqueue_impl(int msg) {
            call_tracker::derived_calls++;
            // Async: would queue to mailbox, but for test just call behavior
            static_cast<Actor*>(this)->behavior(msg);
            return {enqueue_result::success, true};  // needs_scheduling = true
        }

    protected:
        actor_with_mailbox() = default;
        ~actor_with_mailbox() = default;
    };

    // ========================================================================
    // Type-erased address (like address_t) - captures enqueue_impl via lambda
    // ========================================================================
    class test_address {
    public:
        using enqueue_fn_t = std::pair<enqueue_result, bool>(*)(void*, int);

        template<typename Target>
        explicit test_address(Target* ptr)
            : ptr_(ptr)
            , enqueue_fn_(+[](void* p, int msg) {
                  // Static cast to concrete type - no virtual call!
                  return static_cast<Target*>(p)->enqueue_impl(msg);
              }) {}

        std::pair<enqueue_result, bool> enqueue(int msg) {
            return enqueue_fn_(ptr_, msg);
        }

    private:
        void* ptr_;
        enqueue_fn_t enqueue_fn_;
    };

    // ========================================================================
    // Concrete sync actor (directly inherits base_mixin)
    // ========================================================================
    class sync_actor final : public base_mixin<sync_actor> {
    public:
        int last_msg = 0;
        void behavior(int msg) { last_msg = msg; }
    };

    // ========================================================================
    // Concrete async actor (inherits actor_with_mailbox which inherits base_mixin)
    // ========================================================================
    class async_actor final : public actor_with_mailbox<async_actor> {
    public:
        int last_msg = 0;
        void behavior(int msg) { last_msg = msg; }
    };

} // anonymous namespace

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("CRTP enqueue_impl without virtual methods", "[crtp][enqueue]") {

    SECTION("Direct call to sync_actor uses base_mixin::enqueue_impl") {
        call_tracker::reset();
        sync_actor actor;

        auto [result, needs_sched] = actor.enqueue_impl(42);

        REQUIRE(result == enqueue_result::success);
        REQUIRE(needs_sched == false);  // sync returns false
        REQUIRE(actor.last_msg == 42);
        REQUIRE(call_tracker::base_calls == 1);
        REQUIRE(call_tracker::derived_calls == 0);
    }

    SECTION("Direct call to async_actor uses actor_with_mailbox::enqueue_impl (hides base)") {
        call_tracker::reset();
        async_actor actor;

        auto [result, needs_sched] = actor.enqueue_impl(42);

        REQUIRE(result == enqueue_result::success);
        REQUIRE(needs_sched == true);  // async returns true
        REQUIRE(actor.last_msg == 42);
        REQUIRE(call_tracker::base_calls == 0);  // base NOT called
        REQUIRE(call_tracker::derived_calls == 1);
    }

    SECTION("test_address with sync_actor calls base_mixin::enqueue_impl") {
        call_tracker::reset();
        sync_actor actor;
        test_address addr(&actor);

        auto [result, needs_sched] = addr.enqueue(100);

        REQUIRE(result == enqueue_result::success);
        REQUIRE(needs_sched == false);  // sync
        REQUIRE(actor.last_msg == 100);
        REQUIRE(call_tracker::base_calls == 1);
        REQUIRE(call_tracker::derived_calls == 0);
    }

    SECTION("test_address with async_actor calls actor_with_mailbox::enqueue_impl") {
        call_tracker::reset();
        async_actor actor;
        test_address addr(&actor);

        auto [result, needs_sched] = addr.enqueue(200);

        REQUIRE(result == enqueue_result::success);
        REQUIRE(needs_sched == true);  // async
        REQUIRE(actor.last_msg == 200);
        REQUIRE(call_tracker::base_calls == 0);  // base NOT called
        REQUIRE(call_tracker::derived_calls == 1);
    }

    SECTION("No virtual table - sizeof check") {
        // If there were virtual methods, sizeof would include vtable pointer
        // base_mixin has no virtual methods, so no vtable

        // sync_actor only has int last_msg (4 bytes) + possible padding
        // No vtable pointer (typically 8 bytes on 64-bit)
        REQUIRE(sizeof(sync_actor) == sizeof(int));

        // async_actor same - no vtable
        REQUIRE(sizeof(async_actor) == sizeof(int));
    }
}

TEST_CASE("Type erasure preserves correct method binding", "[crtp][type-erasure]") {

    SECTION("Store different actor types as test_address and call correct method") {
        call_tracker::reset();

        sync_actor sync;
        async_actor async;

        // Create addresses - lambda captures concrete type at compile time
        test_address sync_addr(&sync);
        test_address async_addr(&async);

        // Call through type-erased interface
        sync_addr.enqueue(1);
        async_addr.enqueue(2);

        // Verify correct methods were called
        REQUIRE(call_tracker::base_calls == 1);    // sync_actor -> base
        REQUIRE(call_tracker::derived_calls == 1); // async_actor -> derived

        REQUIRE(sync.last_msg == 1);
        REQUIRE(async.last_msg == 2);
    }
}