#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/detail/promise_concepts.hpp>
#include <vector>

// ============================================================================
// Test: unique_future type erasure to void
// ============================================================================

TEST_CASE("unique_future type erasure - int to void") {
    auto* resource = std::pmr::get_default_resource();

    actor_zeta::promise<int> p(resource);
    p.set_value(42);
    auto typed_future = p.get_future();

    REQUIRE(typed_future.available());

    // Type erasure: unique_future<int> -> unique_future<void>
    actor_zeta::unique_future<void> erased = std::move(typed_future);

    REQUIRE(erased.available());
    REQUIRE(erased.valid());

    // Original is now invalid
    REQUIRE(!typed_future.valid());
}

TEST_CASE("unique_future type erasure - string to void") {
    auto* resource = std::pmr::get_default_resource();

    actor_zeta::promise<std::string> p(resource);
    p.set_value("hello");
    auto typed_future = p.get_future();

    REQUIRE(typed_future.available());

    // Type erasure: unique_future<string> -> unique_future<void>
    actor_zeta::unique_future<void> erased = std::move(typed_future);

    REQUIRE(erased.available());
}

// ============================================================================
// Test: from_state() factory method
// ============================================================================

TEST_CASE("from_state - create future from raw state") {
    auto* resource = std::pmr::get_default_resource();

    // Create a promise and get its state
    actor_zeta::promise<int> p(resource);
    p.set_value(100);
    auto original = p.get_future();

    // Get raw state
    auto* state = original.internal_release_state();

    // Create new future from state using from_state()
    auto restored = actor_zeta::unique_future<int>::from_state(
        static_cast<actor_zeta::detail::future_state_base*>(state),
        resource,
        false  // don't add ref, we already own it
    );

    REQUIRE(restored.available());
    REQUIRE(std::move(restored).get() == 100);
}

// ============================================================================
// Test: vector of futures (key requirement)
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

    void behavior(actor_zeta::mailbox::message* msg) {
        if (msg->command() == actor_zeta::msg_id<worker_actor, &worker_actor::compute>) {
            actor_zeta::dispatch(this, &worker_actor::compute, msg);
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

    futures.push_back(actor_zeta::send(
        worker1.get(),
        actor_zeta::address_t::empty_address(),
        &worker_actor::compute,
        10
    ));
    futures.push_back(actor_zeta::send(
        worker2.get(),
        actor_zeta::address_t::empty_address(),
        &worker_actor::compute,
        10
    ));
    futures.push_back(actor_zeta::send(
        worker3.get(),
        actor_zeta::address_t::empty_address(),
        &worker_actor::compute,
        10
    ));

    // Process messages
    worker1->resume(10);
    worker2->resume(10);
    worker3->resume(10);

    // Verify results
    REQUIRE(futures[0].available());
    REQUIRE(futures[1].available());
    REQUIRE(futures[2].available());

    REQUIRE(std::move(futures[0]).get() == 20);  // 10 * 2
    REQUIRE(std::move(futures[1]).get() == 30);  // 10 * 3
    REQUIRE(std::move(futures[2]).get() == 50);  // 10 * 5
}

// ============================================================================
// Test: vector of type-erased futures
// ============================================================================

TEST_CASE("vector of type-erased futures") {
    auto* resource = std::pmr::get_default_resource();

    // Create futures of different types
    actor_zeta::promise<int> p1(resource);
    p1.set_value(42);

    actor_zeta::promise<std::string> p2(resource);
    p2.set_value("hello");

    actor_zeta::promise<double> p3(resource);
    p3.set_value(3.14);

    // Type-erase all into vector<unique_future<void>>
    std::vector<actor_zeta::unique_future<void>> futures;
    futures.reserve(3);

    futures.push_back(p1.get_future());  // int -> void
    futures.push_back(p2.get_future());  // string -> void
    futures.push_back(p3.get_future());  // double -> void

    // All should be available
    for (auto& f : futures) {
        REQUIRE(f.available());
    }
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

    void behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<old_style_actor, &old_style_actor::compute>:
                actor_zeta::dispatch(this, &old_style_actor::compute, msg);
                break;
            case actor_zeta::msg_id<old_style_actor, &old_style_actor::do_work>:
                actor_zeta::dispatch(this, &old_style_actor::do_work, msg);
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
    auto future1 = actor_zeta::send(
        actor.get(),
        actor_zeta::address_t::empty_address(),
        &old_style_actor::compute,
        21
    );

    actor->resume(10);

    REQUIRE(future1.available());
    REQUIRE(std::move(future1).get() == 42);
    REQUIRE(actor->call_count() == 1);

    // Send to void method
    auto future2 = actor_zeta::send(
        actor.get(),
        actor_zeta::address_t::empty_address(),
        &old_style_actor::do_work
    );

    actor->resume(10);

    REQUIRE(future2.available());
    std::move(future2).get();  // Should not crash
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
    void behavior(actor_zeta::mailbox::message*) {}
};

class actor_with_custom_promise : public actor_zeta::basic_actor<actor_with_custom_promise> {
public:
    explicit actor_with_custom_promise(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<actor_with_custom_promise>(ptr) {}

    // Custom promise_type template alias
    template<typename T>
    using promise_type = typename actor_zeta::unique_future<T>::promise_type;

    using dispatch_traits = actor_zeta::dispatch_traits<>;
    void behavior(actor_zeta::mailbox::message*) {}
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
    REQUIRE(void_future.available());

    // int with value
    auto int_future = actor_zeta::make_ready_future<int>(resource, 42);
    REQUIRE(int_future.available());
    REQUIRE(std::move(int_future).get() == 42);

    // string with value
    auto str_future = actor_zeta::make_ready_future<std::string>(resource, std::string("test"));
    REQUIRE(str_future.available());
    REQUIRE(std::move(str_future).get() == "test");

    // int default constructed
    auto int_default = actor_zeta::make_ready_future<int>(resource);
    REQUIRE(int_default.available());
    REQUIRE(std::move(int_default).get() == 0);
}