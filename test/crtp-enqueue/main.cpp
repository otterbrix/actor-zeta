#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <iostream>
#include <memory_resource>
#include <utility>

namespace {

    enum class enqueue_result { success, queue_closed };

    struct call_tracker {
        static int base_calls;
        static int derived_calls;
        static void reset() { base_calls = 0; derived_calls = 0; }
    };
    int call_tracker::base_calls = 0;
    int call_tracker::derived_calls = 0;

    template<typename Derived>
    class base_mixin {
    public:
        std::pmr::memory_resource* resource() const {
            return std::pmr::get_default_resource();
        }

        std::pair<bool, enqueue_result> enqueue_impl(int msg) {
            call_tracker::base_calls++;
            static_cast<Derived*>(this)->behavior(msg);
            return {false, enqueue_result::success};
        }

    protected:
        base_mixin() = default;
        ~base_mixin() = default;
    };

    template<typename Actor>
    class actor_with_mailbox : public base_mixin<Actor> {
    public:
        std::pair<bool, enqueue_result> enqueue_impl(int msg) {
            call_tracker::derived_calls++;
            static_cast<Actor*>(this)->behavior(msg);
            return {true, enqueue_result::success};
        }

    protected:
        actor_with_mailbox() = default;
        ~actor_with_mailbox() = default;
    };

    class test_address {
    public:
        using enqueue_fn_t = std::pair<bool, enqueue_result>(*)(void*, int);

        template<typename Target>
        explicit test_address(Target* ptr)
            : ptr_(ptr)
            , enqueue_fn_(+[](void* p, int msg) {
                  return static_cast<Target*>(p)->enqueue_impl(msg);
              }) {}

        std::pair<bool, enqueue_result> enqueue(int msg) {
            return enqueue_fn_(ptr_, msg);
        }

    private:
        void* ptr_;
        enqueue_fn_t enqueue_fn_;
    };

    class sync_actor final : public base_mixin<sync_actor> {
    public:
        int last_msg = 0;
        void behavior(int msg) { last_msg = msg; }
    };

    class async_actor final : public actor_with_mailbox<async_actor> {
    public:
        int last_msg = 0;
        void behavior(int msg) { last_msg = msg; }
    };

} // anonymous namespace

TEST_CASE("CRTP enqueue_impl without virtual methods", "[crtp][enqueue]") {

    SECTION("Direct call to sync_actor uses base_mixin::enqueue_impl") {
        call_tracker::reset();
        sync_actor actor;

        auto [needs_sched, result] = actor.enqueue_impl(42);

        REQUIRE(result == enqueue_result::success);
        REQUIRE(needs_sched == false);
        REQUIRE(actor.last_msg == 42);
        REQUIRE(call_tracker::base_calls == 1);
        REQUIRE(call_tracker::derived_calls == 0);
    }

    SECTION("Direct call to async_actor uses actor_with_mailbox::enqueue_impl (hides base)") {
        call_tracker::reset();
        async_actor actor;

        auto [needs_sched, result] = actor.enqueue_impl(42);

        REQUIRE(result == enqueue_result::success);
        REQUIRE(needs_sched == true);
        REQUIRE(actor.last_msg == 42);
        REQUIRE(call_tracker::base_calls == 0);
        REQUIRE(call_tracker::derived_calls == 1);
    }

    SECTION("test_address with sync_actor calls base_mixin::enqueue_impl") {
        call_tracker::reset();
        sync_actor actor;
        test_address addr(&actor);

        auto [needs_sched, result] = addr.enqueue(100);

        REQUIRE(result == enqueue_result::success);
        REQUIRE(needs_sched == false);
        REQUIRE(actor.last_msg == 100);
        REQUIRE(call_tracker::base_calls == 1);
        REQUIRE(call_tracker::derived_calls == 0);
    }

    SECTION("test_address with async_actor calls actor_with_mailbox::enqueue_impl") {
        call_tracker::reset();
        async_actor actor;
        test_address addr(&actor);

        auto [needs_sched, result] = addr.enqueue(200);

        REQUIRE(result == enqueue_result::success);
        REQUIRE(needs_sched == true);
        REQUIRE(actor.last_msg == 200);
        REQUIRE(call_tracker::base_calls == 0);
        REQUIRE(call_tracker::derived_calls == 1);
    }

    SECTION("No virtual table - sizeof check") {
        REQUIRE(sizeof(sync_actor) == sizeof(int));

        REQUIRE(sizeof(async_actor) == sizeof(int));
    }
}

TEST_CASE("Type erasure preserves correct method binding", "[crtp][type-erasure]") {

    SECTION("Store different actor types as test_address and call correct method") {
        call_tracker::reset();

        sync_actor sync;
        async_actor async;

        test_address sync_addr(&sync);
        test_address async_addr(&async);

        sync_addr.enqueue(1);
        async_addr.enqueue(2);

        REQUIRE(call_tracker::base_calls == 1);
        REQUIRE(call_tracker::derived_calls == 1);

        REQUIRE(sync.last_msg == 1);
        REQUIRE(async.last_msg == 2);
    }
}