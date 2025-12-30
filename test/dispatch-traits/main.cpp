#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>

class test_actor final : public actor_zeta::basic_actor<test_actor> {
public:
    explicit test_actor(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<test_actor>(ptr)
        , call_count1_(0)
        , call_count2_(0)
        , call_count3_(0) {
    }

    // All methods must be coroutines (use co_return)
    actor_zeta::unique_future<void> method1(int value) {
        ++call_count1_;
        last_value1_ = value;
        co_return;
    }

    actor_zeta::unique_future<void> method2(std::string text) {
        ++call_count2_;
        last_value2_ = text;
        co_return;
    }

    actor_zeta::unique_future<void> method3() {
        ++call_count3_;
        co_return;
    }

    // New dispatch_traits syntax - AFTER method definitions!
    using dispatch_traits = actor_zeta::dispatch_traits<
        &test_actor::method1,
        &test_actor::method2,
        &test_actor::method3
    >;

    void behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<test_actor, &test_actor::method1>:
                dispatch(this, &test_actor::method1, msg);
                break;
            case actor_zeta::msg_id<test_actor, &test_actor::method2>:
                dispatch(this, &test_actor::method2, msg);
                break;
            case actor_zeta::msg_id<test_actor, &test_actor::method3>:
                dispatch(this, &test_actor::method3, msg);
                break;
        }
    }

    int call_count1() const { return call_count1_; }
    int call_count2() const { return call_count2_; }
    int call_count3() const { return call_count3_; }
    int last_value1() const { return last_value1_; }
    std::string last_value2() const { return last_value2_; }

private:
    int call_count1_;
    int call_count2_;
    int call_count3_;
    int last_value1_;
    std::string last_value2_;
};

TEST_CASE("dispatch_traits - compile-time msg_id generation") {
    // Verify that msg_id is a compile-time constant
    constexpr auto id1 = actor_zeta::msg_id<test_actor, &test_actor::method1>;
    constexpr auto id2 = actor_zeta::msg_id<test_actor, &test_actor::method2>;
    constexpr auto id3 = actor_zeta::msg_id<test_actor, &test_actor::method3>;

    // message_id has default priority flag, so check that ActionId in lower bits is correct
    REQUIRE((id1 & 0xFFFFFFFF) == 0);
    REQUIRE((id2 & 0xFFFFFFFF) == 1);
    REQUIRE((id3 & 0xFFFFFFFF) == 2);
}

TEST_CASE("dispatch_traits - unique message IDs") {
    constexpr auto id1 = actor_zeta::msg_id<test_actor, &test_actor::method1>;
    constexpr auto id2 = actor_zeta::msg_id<test_actor, &test_actor::method2>;
    constexpr auto id3 = actor_zeta::msg_id<test_actor, &test_actor::method3>;

    REQUIRE(id1 != id2);
    REQUIRE(id2 != id3);
    REQUIRE(id1 != id3);
}

TEST_CASE("dispatch_traits - sequential indexing") {
    // ActionId should be sequential: 0, 1, 2, ...
    constexpr auto id1 = actor_zeta::msg_id<test_actor, &test_actor::method1>;
    constexpr auto id2 = actor_zeta::msg_id<test_actor, &test_actor::method2>;
    constexpr auto id3 = actor_zeta::msg_id<test_actor, &test_actor::method3>;

    // Extract ActionId from lower bits
    constexpr uint64_t action1 = id1 & 0xFFFFFFFF;
    constexpr uint64_t action2 = id2 & 0xFFFFFFFF;
    constexpr uint64_t action3 = id3 & 0xFFFFFFFF;

    REQUIRE(action1 == 0);
    REQUIRE(action2 == 1);
    REQUIRE(action3 == 2);
    REQUIRE(action2 == action1 + 1);
    REQUIRE(action3 == action2 + 1);
}

TEST_CASE("dispatch_traits - simple one-line syntax") {
    // This test verifies that the new syntax compiles
    // using dispatch_traits = actor_zeta::dispatch_traits<&Actor::method1, ...>;

    auto* resource =std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<test_actor>(resource);

    REQUIRE(actor != nullptr);
    REQUIRE(actor->call_count1() == 0);
    REQUIRE(actor->call_count2() == 0);
    REQUIRE(actor->call_count3() == 0);
}

// ============================================================================
// Sync dispatch policy tests
// ============================================================================

// Sync actor uses actor_mixin (not basic_actor/cooperative_actor which is async)
class sync_actor final : public actor_zeta::actor::actor_mixin<sync_actor> {
public:
    template<typename T> using unique_future = actor_zeta::unique_future<T>;

    explicit sync_actor(std::pmr::memory_resource* ptr)
        : actor_zeta::actor::actor_mixin<sync_actor>()
        , resource_(ptr)
        , call_count_(0) {
    }

    std::pmr::memory_resource* resource() const noexcept { return resource_; }

    actor_zeta::unique_future<int> compute(int value) {
        ++call_count_;
        co_return value * 2;
    }

    // Sync actor - behavior() called immediately via actor_mixin::enqueue_impl
    using dispatch_traits = actor_zeta::dispatch_traits<
        &sync_actor::compute
    >;

    void behavior(actor_zeta::mailbox::message* msg) {
        if (msg->command() == actor_zeta::msg_id<sync_actor, &sync_actor::compute>) {
            actor_zeta::dispatch(this, &sync_actor::compute, msg);
        }
    }

    int call_count() const { return call_count_; }

private:
    std::pmr::memory_resource* resource_;
    int call_count_;
};

TEST_CASE("sync actor - actor_mixin processes immediately") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<sync_actor>(resource);

    REQUIRE(actor != nullptr);

    // Send message - actor_mixin::enqueue_impl calls behavior() immediately
    auto future = actor_zeta::send(
        actor.get(),
        actor_zeta::address_t::empty_address(),
        &sync_actor::compute,
        21);

    // With actor_mixin, result should be available immediately (no resume needed)
    REQUIRE(future.available());
    REQUIRE(std::move(future).get() == 42);
    REQUIRE(actor->call_count() == 1);
}

// ============================================================================
// Optional send tests (send without target - detected by argument count)
// ============================================================================

class optional_test_actor final : public actor_zeta::basic_actor<optional_test_actor> {
public:
    explicit optional_test_actor(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<optional_test_actor>(ptr)
        , call_count_(0) {
    }

    actor_zeta::unique_future<int> process(int value) {
        ++call_count_;
        co_return value + 10;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &optional_test_actor::process
    >;

    void behavior(actor_zeta::mailbox::message* msg) {
        if (msg->command() == actor_zeta::msg_id<optional_test_actor, &optional_test_actor::process>) {
            actor_zeta::dispatch(this, &optional_test_actor::process, msg);
        }
    }

    int call_count() const { return call_count_; }

private:
    int call_count_;
};

TEST_CASE("optional send - returns ready future with default value") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<optional_test_actor>(resource);

    REQUIRE(actor != nullptr);

    // Optional send - no target, just sender and method
    // Uses sender.resource() to create ready future
    auto future = actor_zeta::send(
        actor->address(),  // sender with valid resource
        &optional_test_actor::process,
        5);

    // Result is immediately available with default value
    REQUIRE(future.available());
    REQUIRE(std::move(future).get() == 0);  // Default int is 0
    REQUIRE(actor->call_count() == 0);  // Method was not called
}

TEST_CASE("optional send - address_t stores resource from actor") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<optional_test_actor>(resource);

    REQUIRE(actor != nullptr);

    // Get address from actor
    auto addr = actor->address();

    // Verify address has resource
    REQUIRE(addr.resource() != nullptr);
    REQUIRE(addr.resource() == resource);
}

// ============================================================================
// Scheduler type alias tests
// ============================================================================

#include <actor-zeta/scheduler.hpp>

TEST_CASE("scheduler type aliases") {
    // Verify scheduler_ptr is std::unique_ptr<sharing_scheduler>
    static_assert(std::is_same_v<
        actor_zeta::scheduler_ptr,
        std::unique_ptr<actor_zeta::sharing_scheduler>
    >, "scheduler_ptr should be unique_ptr<sharing_scheduler>");

    // Verify scheduler_raw is sharing_scheduler*
    static_assert(std::is_same_v<
        actor_zeta::scheduler_raw,
        actor_zeta::sharing_scheduler*
    >, "scheduler_raw should be sharing_scheduler*");

    REQUIRE(true); // Compile-time checks passed
}


// ============================================================================
// make_ready_future tests
// ============================================================================

TEST_CASE("make_ready_future - void") {
    auto* resource = std::pmr::get_default_resource();

    auto future = actor_zeta::make_ready_future(resource);

    REQUIRE(future.available());
    std::move(future).get(); // Should not throw/crash
}

TEST_CASE("make_ready_future - int with value") {
    auto* resource = std::pmr::get_default_resource();

    auto future = actor_zeta::make_ready_future<int>(resource, 42);

    REQUIRE(future.available());
    REQUIRE(std::move(future).get() == 42);
}

TEST_CASE("make_ready_future - string with value") {
    auto* resource = std::pmr::get_default_resource();

    auto future = actor_zeta::make_ready_future<std::string>(resource, std::string("hello"));

    REQUIRE(future.available());
    REQUIRE(std::move(future).get() == "hello");
}

TEST_CASE("make_ready_future - int default constructed") {
    auto* resource = std::pmr::get_default_resource();

    auto future = actor_zeta::make_ready_future<int>(resource);

    REQUIRE(future.available());
    REQUIRE(std::move(future).get() == 0); // Default int is 0
}

TEST_CASE("make_ready_future - string default constructed") {
    auto* resource = std::pmr::get_default_resource();

    auto future = actor_zeta::make_ready_future<std::string>(resource);

    REQUIRE(future.available());
    REQUIRE(std::move(future).get().empty()); // Default string is empty
}

// ============================================================================
// make_error tests
// ============================================================================

TEST_CASE("make_error - int future") {
    auto* resource = std::pmr::get_default_resource();

    auto future = actor_zeta::make_error<int>(resource, std::make_error_code(std::errc::invalid_argument));
    
    REQUIRE(future.failed());
}

TEST_CASE("make_error - void future") {
    auto* resource = std::pmr::get_default_resource();

    auto future = actor_zeta::make_error<void>(resource, std::make_error_code(std::errc::operation_canceled));

    REQUIRE(future.failed());
}

TEST_CASE("make_error - error code is preserved") {
    auto* resource = std::pmr::get_default_resource();
    auto expected_error = std::make_error_code(std::errc::invalid_argument);

    auto future = actor_zeta::make_error<int>(resource, expected_error);

    REQUIRE(future.failed());
    REQUIRE(future.error() == expected_error);
    REQUIRE(future.error().value() == static_cast<int>(std::errc::invalid_argument));
}

// ============================================================================
// address_t copy/move semantics tests
// ============================================================================

TEST_CASE("address_t - copy preserves resource") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<optional_test_actor>(resource);

    auto addr1 = actor->address();
    REQUIRE(addr1.resource() == resource);

    // Copy constructor
    auto addr2 = addr1;
    REQUIRE(addr2.resource() == resource);
    REQUIRE(addr2.get() == addr1.get());
}

TEST_CASE("address_t - move transfers resource") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<optional_test_actor>(resource);

    auto addr1 = actor->address();
    void* original_ptr = addr1.get();

    // Move constructor
    auto addr2 = std::move(addr1);
    REQUIRE(addr2.resource() == resource);
    REQUIRE(addr2.get() == original_ptr);

    // After move, addr1 should be empty
    REQUIRE(addr1.get() == nullptr);
    REQUIRE(addr1.resource() == nullptr);
}

TEST_CASE("address_t - copy assignment preserves resource") {
    auto* resource = std::pmr::get_default_resource();
    auto actor1 = actor_zeta::spawn<optional_test_actor>(resource);
    auto actor2 = actor_zeta::spawn<optional_test_actor>(resource);

    auto addr1 = actor1->address();
    auto addr2 = actor2->address();

    void* ptr1 = addr1.get();

    // Copy assignment
    addr2 = addr1;
    REQUIRE(addr2.resource() == resource);
    REQUIRE(addr2.get() == ptr1);
}

TEST_CASE("address_t - move assignment transfers resource") {
    auto* resource = std::pmr::get_default_resource();
    auto actor1 = actor_zeta::spawn<optional_test_actor>(resource);
    auto actor2 = actor_zeta::spawn<optional_test_actor>(resource);

    auto addr1 = actor1->address();
    auto addr2 = actor2->address();

    void* ptr1 = addr1.get();
    void* ptr2 = addr2.get();

    // Move assignment uses swap - addr1 gets old addr2 value
    addr2 = std::move(addr1);
    REQUIRE(addr2.resource() == resource);
    REQUIRE(addr2.get() == ptr1);
    // After swap, addr1 has old addr2 value
    REQUIRE(addr1.get() == ptr2);
    REQUIRE(addr1.resource() == resource);
}

TEST_CASE("address_t - empty_address has nullptr resource") {
    // Document: empty_address() has nullptr resource
    // This is intentional - use actor->address() when resource is needed
    auto addr = actor_zeta::address_t::empty_address();

    REQUIRE(addr.get() == nullptr);
    REQUIRE(addr.resource() == nullptr);
    REQUIRE(!addr);  // operator bool returns false
}

// ============================================================================
// Sync dispatch with multiple methods
// ============================================================================

// Sync actor uses actor_mixin (not basic_actor/cooperative_actor which is async)
class sync_multi_actor final : public actor_zeta::actor::actor_mixin<sync_multi_actor> {
public:
    template<typename T> using unique_future = actor_zeta::unique_future<T>;

    explicit sync_multi_actor(std::pmr::memory_resource* ptr)
        : actor_zeta::actor::actor_mixin<sync_multi_actor>()
        , resource_(ptr)
        , add_count_(0)
        , multiply_count_(0) {
    }

    std::pmr::memory_resource* resource() const noexcept { return resource_; }

    actor_zeta::unique_future<int> add(int a, int b) {
        ++add_count_;
        co_return a + b;
    }

    actor_zeta::unique_future<int> multiply(int a, int b) {
        ++multiply_count_;
        co_return a * b;
    }

    actor_zeta::unique_future<void> reset() {
        add_count_ = 0;
        multiply_count_ = 0;
        co_return;
    }

    // Sync actor - actor_mixin processes immediately (no mailbox)
    using dispatch_traits = actor_zeta::dispatch_traits<
        &sync_multi_actor::add,
        &sync_multi_actor::multiply,
        &sync_multi_actor::reset
    >;

    void behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<sync_multi_actor, &sync_multi_actor::add>:
                actor_zeta::dispatch(this, &sync_multi_actor::add, msg);
                break;
            case actor_zeta::msg_id<sync_multi_actor, &sync_multi_actor::multiply>:
                actor_zeta::dispatch(this, &sync_multi_actor::multiply, msg);
                break;
            case actor_zeta::msg_id<sync_multi_actor, &sync_multi_actor::reset>:
                actor_zeta::dispatch(this, &sync_multi_actor::reset, msg);
                break;
        }
    }

    int add_count() const { return add_count_; }
    int multiply_count() const { return multiply_count_; }

private:
    std::pmr::memory_resource* resource_;
    int add_count_;
    int multiply_count_;
};

TEST_CASE("sync actor - multiple methods") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<sync_multi_actor>(resource);

    REQUIRE(actor != nullptr);

    // Test add method
    auto future1 = actor_zeta::send(
        actor.get(),
        actor_zeta::address_t::empty_address(),
        &sync_multi_actor::add,
        10, 20);

    REQUIRE(future1.available());
    REQUIRE(std::move(future1).get() == 30);
    REQUIRE(actor->add_count() == 1);

    // Test multiply method
    auto future2 = actor_zeta::send(
        actor.get(),
        actor_zeta::address_t::empty_address(),
        &sync_multi_actor::multiply,
        6, 7);

    REQUIRE(future2.available());
    REQUIRE(std::move(future2).get() == 42);
    REQUIRE(actor->multiply_count() == 1);

    // Test void method
    auto future3 = actor_zeta::send(
        actor.get(),
        actor_zeta::address_t::empty_address(),
        &sync_multi_actor::reset);

    REQUIRE(future3.available());
    std::move(future3).get();  // Should not crash
    REQUIRE(actor->add_count() == 0);
    REQUIRE(actor->multiply_count() == 0);
}

TEST_CASE("sync_dispatch - msg_id sequential for multiple methods") {
    // Verify msg_ids are sequential even with sync policy
    constexpr auto id1 = actor_zeta::msg_id<sync_multi_actor, &sync_multi_actor::add>;
    constexpr auto id2 = actor_zeta::msg_id<sync_multi_actor, &sync_multi_actor::multiply>;
    constexpr auto id3 = actor_zeta::msg_id<sync_multi_actor, &sync_multi_actor::reset>;

    // Extract ActionId from lower bits
    constexpr uint64_t action1 = id1 & 0xFFFFFFFF;
    constexpr uint64_t action2 = id2 & 0xFFFFFFFF;
    constexpr uint64_t action3 = id3 & 0xFFFFFFFF;

    REQUIRE(action1 == 0);
    REQUIRE(action2 == 1);
    REQUIRE(action3 == 2);
}

// ============================================================================
// Optional send with different types
// ============================================================================

class optional_void_actor final : public actor_zeta::basic_actor<optional_void_actor> {
public:
    explicit optional_void_actor(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<optional_void_actor>(ptr)
        , called_(false) {
    }

    actor_zeta::unique_future<void> do_work() {
        called_ = true;
        co_return;
    }

    actor_zeta::unique_future<std::string> get_name() {
        co_return std::string("test_name");
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &optional_void_actor::do_work,
        &optional_void_actor::get_name
    >;

    void behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<optional_void_actor, &optional_void_actor::do_work>:
                actor_zeta::dispatch(this, &optional_void_actor::do_work, msg);
                break;
            case actor_zeta::msg_id<optional_void_actor, &optional_void_actor::get_name>:
                actor_zeta::dispatch(this, &optional_void_actor::get_name, msg);
                break;
        }
    }

    bool called() const { return called_; }

private:
    bool called_;
};

TEST_CASE("optional send - void return type") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<optional_void_actor>(resource);

    REQUIRE(actor != nullptr);

    // Optional send for void method - should return ready void future
    auto future = actor_zeta::send(
        actor->address(),
        &optional_void_actor::do_work);

    REQUIRE(future.available());
    std::move(future).get();  // Should not crash
    REQUIRE(actor->called() == false);  // Method was not called
}

TEST_CASE("optional send - string return type") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<optional_void_actor>(resource);

    REQUIRE(actor != nullptr);

    // Optional send for string method - returns empty string
    auto future = actor_zeta::send(
        actor->address(),
        &optional_void_actor::get_name);

    REQUIRE(future.available());
    REQUIRE(std::move(future).get().empty());  // Default string is empty
}

TEST_CASE("dispatch_traits - empty traits") {
    // Empty dispatch_traits should be valid
    using empty_traits = actor_zeta::dispatch_traits<>;
    static_assert(std::is_same_v<empty_traits::methods, actor_zeta::type_traits::type_list<>>,
                  "Empty dispatch_traits should have empty methods list");

    REQUIRE(true);  // Compile-time check passed
}