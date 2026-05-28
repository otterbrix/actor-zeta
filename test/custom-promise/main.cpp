#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/detail/promise_concepts.hpp>
#include <vector>

// ============================================================================
// Test: promise basic API
// ============================================================================

TEST_CASE("promise<int> - basic set_value and get_future") {
    auto* resource = std::pmr::get_default_resource();

    actor_zeta::promise<int> p(resource);
    REQUIRE(p.valid());

    auto future = p.get_future();
    REQUIRE(future.valid());
    REQUIRE_FALSE(future.is_ready());

    p.set_value(42);
    REQUIRE_FALSE(p.valid());  // Promise invalidated after set_value

    REQUIRE(future.is_ready());
    REQUIRE(std::move(future).take_ready() == 42);
}

TEST_CASE("promise<void> - basic set_value and get_future") {
    auto* resource = std::pmr::get_default_resource();

    actor_zeta::promise<void> p(resource);
    REQUIRE(p.valid());

    auto future = p.get_future();
    REQUIRE(future.valid());
    REQUIRE_FALSE(future.is_ready());

    p.set_value();
    REQUIRE_FALSE(p.valid());

    REQUIRE(future.is_ready());
}

TEST_CASE("promise<string> - complex type") {
    auto* resource = std::pmr::get_default_resource();

    actor_zeta::promise<std::string> p(resource);
    auto future = p.get_future();

    p.set_value("hello world");

    REQUIRE(future.is_ready());
    REQUIRE(std::move(future).take_ready() == "hello world");
}

// ============================================================================
// Test: vector of futures - key requirement!
// ============================================================================

class worker_actor final : public actor_zeta::basic_actor<worker_actor> {
public:
    explicit worker_actor(std::pmr::memory_resource* ptr, int id)
        : actor_zeta::basic_actor<worker_actor>(ptr)
        , id_(id) {}

    actor_zeta::unique_future<int> compute(int x) {
        co_return x * id_;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&worker_actor::compute>;

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        if (msg->command() == actor_zeta::msg_id<worker_actor, &worker_actor::compute>) {
            co_await actor_zeta::dispatch(this, &worker_actor::compute, msg);
        }
    }

private:
    int id_;
};

TEST_CASE("vector of futures - same type from different actors") {
    auto* resource = std::pmr::get_default_resource();

    // Create multiple workers
    auto worker1 = actor_zeta::spawn<worker_actor>(resource, 2);
    auto worker2 = actor_zeta::spawn<worker_actor>(resource, 3);
    auto worker3 = actor_zeta::spawn<worker_actor>(resource, 5);

    // Collect futures in a vector - key requirement!
    std::vector<actor_zeta::unique_future<int>> futures;
    futures.reserve(3);  // Important: reserve to avoid reallocation

    {
        auto [needs_sched, future] = actor_zeta::send(
            worker1.get(),
            &worker_actor::compute,
            10
        );
        futures.push_back(std::move(future));
    }
    {
        auto [needs_sched, future] = actor_zeta::send(
            worker2.get(),
            &worker_actor::compute,
            10
        );
        futures.push_back(std::move(future));
    }
    {
        auto [needs_sched, future] = actor_zeta::send(
            worker3.get(),
            &worker_actor::compute,
            10
        );
        futures.push_back(std::move(future));
    }

    // Process messages
    worker1->resume(10);
    worker2->resume(10);
    worker3->resume(10);

    // Verify results
    REQUIRE(futures[0].is_ready());
    REQUIRE(futures[1].is_ready());
    REQUIRE(futures[2].is_ready());

    REQUIRE(std::move(futures[0]).take_ready() == 20);  // 10 * 2
    REQUIRE(std::move(futures[1]).take_ready() == 30);  // 10 * 3
    REQUIRE(std::move(futures[2]).take_ready() == 50);  // 10 * 5
}

// ============================================================================
// Test: Concepts validation
// ============================================================================

TEST_CASE("promise concepts - valid_future_value_type") {
    // void is valid
    static_assert(actor_zeta::detail::valid_future_value_type<void>);

    // Primitive types are valid
    static_assert(actor_zeta::detail::valid_future_value_type<int>);
    static_assert(actor_zeta::detail::valid_future_value_type<double>);

    // Standard library types are valid
    static_assert(actor_zeta::detail::valid_future_value_type<std::string>);
    static_assert(actor_zeta::detail::valid_future_value_type<std::vector<int>>);

    // References are NOT valid
    static_assert(!actor_zeta::detail::valid_future_value_type<int&>);
    static_assert(!actor_zeta::detail::valid_future_value_type<const int&>);

    // const types are NOT valid
    static_assert(!actor_zeta::detail::valid_future_value_type<const int>);

    REQUIRE(true);  // All static_asserts passed
}

// ============================================================================
// Test: Backward compatibility - old actor code works
// ============================================================================

class old_style_actor final : public actor_zeta::basic_actor<old_style_actor> {
public:
    explicit old_style_actor(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<old_style_actor>(ptr)
        , call_count_(0) {}

    // Old-style coroutine method - still works!
    actor_zeta::unique_future<int> compute(int x) {
        ++call_count_;
        co_return x * 2;
    }

    actor_zeta::unique_future<void> do_work() {
        ++call_count_;
        co_return;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &old_style_actor::compute,
        &old_style_actor::do_work
    >;

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<old_style_actor, &old_style_actor::compute>:
                co_await actor_zeta::dispatch(this, &old_style_actor::compute, msg);
                break;
            case actor_zeta::msg_id<old_style_actor, &old_style_actor::do_work>:
                co_await actor_zeta::dispatch(this, &old_style_actor::do_work, msg);
                break;
        }
    }

    int call_count() const { return call_count_; }

private:
    int call_count_;
};

TEST_CASE("backward compatibility - old actor code works") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<old_style_actor>(resource);

    // Send to typed method
    auto [needs_sched1, future1] = actor_zeta::send(
            actor.get(),
            &old_style_actor::compute,
        21
    );

    actor->resume(10);

    REQUIRE(future1.is_ready());
    REQUIRE(std::move(future1).take_ready() == 42);
    REQUIRE(actor->call_count() == 1);

    // Send to void method
    auto [needs_sched2, future2] = actor_zeta::send(
            actor.get(),
            &old_style_actor::do_work
    );

    actor->resume(10);

    REQUIRE(future2.is_ready());
    std::move(future2).take_ready();  // Should not crash
    REQUIRE(actor->call_count() == 2);
}

// ============================================================================
// Test: has_custom_promise_type concept
// ============================================================================

class actor_without_custom_promise : public actor_zeta::basic_actor<actor_without_custom_promise> {
public:
    explicit actor_without_custom_promise(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<actor_without_custom_promise>(ptr) {}

    using dispatch_traits = actor_zeta::dispatch_traits<>;
    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message*) { co_return; }
};

class actor_with_custom_promise : public actor_zeta::basic_actor<actor_with_custom_promise> {
public:
    explicit actor_with_custom_promise(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<actor_with_custom_promise>(ptr) {}

    // Custom promise_type template alias
    template<typename T>
    using promise_type = typename actor_zeta::unique_future<T>::promise_type;

    using dispatch_traits = actor_zeta::dispatch_traits<>;
    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message*) { co_return; }
};

TEST_CASE("has_custom_promise_type concept") {
    // Actor without custom promise_type
    static_assert(!actor_zeta::detail::has_custom_promise_type<actor_without_custom_promise>);

    // Actor with custom promise_type
    static_assert(actor_zeta::detail::has_custom_promise_type<actor_with_custom_promise>);

    REQUIRE(true);  // All static_asserts passed
}

// ============================================================================
// Test: make_ready_future works
// ============================================================================

TEST_CASE("make_ready_future - all overloads") {
    auto* resource = std::pmr::get_default_resource();

    // void
    auto void_future = actor_zeta::make_ready_future(resource);
    REQUIRE(void_future.is_ready());

    // int with value
    auto int_future = actor_zeta::make_ready_future<int>(resource, 42);
    REQUIRE(int_future.is_ready());
    REQUIRE(std::move(int_future).take_ready() == 42);

    // string with value
    auto str_future = actor_zeta::make_ready_future<std::string>(resource, std::string("test"));
    REQUIRE(str_future.is_ready());
    REQUIRE(std::move(str_future).take_ready() == "test");

    // int default constructed
    auto int_default = actor_zeta::make_ready_future<int>(resource);
    REQUIRE(int_default.is_ready());
    REQUIRE(std::move(int_default).take_ready() == 0);
}

// ============================================================================
// Test: Promise destruction without set_value sets error
// ============================================================================

TEST_CASE("promise destruction without set_value - sets broken_pipe") {
    auto* resource = std::pmr::get_default_resource();

    actor_zeta::unique_future<int> future([resource]() {
        actor_zeta::promise<int> p(resource);
        auto f = p.get_future();
        // Promise destroyed without set_value
        return f;
    }());

    // Future should be in failed state with broken_pipe
    REQUIRE(future.is_ready());
    REQUIRE(future.failed());
    REQUIRE(future.error() == std::make_error_code(std::errc::broken_pipe));
}

// ============================================================================
// Test: Promise set_error
// ============================================================================

TEST_CASE("promise set_error - propagates error to future") {
    auto* resource = std::pmr::get_default_resource();

    actor_zeta::promise<int> p(resource);
    auto future = p.get_future();

    p.error(std::make_error_code(std::errc::invalid_argument));

    REQUIRE(future.is_ready());
    REQUIRE(future.failed());
    REQUIRE(future.error() == std::make_error_code(std::errc::invalid_argument));
}
