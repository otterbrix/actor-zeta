#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>

// ============================================================================
// Contract definition
// ============================================================================

struct test_contract {
    actor_zeta::unique_future<void> method1(int);
    actor_zeta::unique_future<void> method2(std::string);
    actor_zeta::unique_future<int> method3();

    using dispatch_traits = actor_zeta::dispatch_traits<
        &test_contract::method1,
        &test_contract::method2,
        &test_contract::method3
    >;

    test_contract() = delete;
};

// ============================================================================
// First implementation
// ============================================================================

class impl_a final : public actor_zeta::basic_actor<impl_a> {
public:
    explicit impl_a(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<impl_a>(ptr)
        , call_count_(0)
        , last_value_(0) {
    }

    actor_zeta::unique_future<void> method1(int value) {
        ++call_count_;
        last_value_ = value;
        co_return;
    }

    actor_zeta::unique_future<void> method2(std::string text) {
        ++call_count_;
        last_text_ = text;
        co_return;
    }

    actor_zeta::unique_future<int> method3() {
        ++call_count_;
        co_return 42;
    }

    // Uses implements<> instead of dispatch_traits<>
    using dispatch_traits = actor_zeta::implements<
        test_contract,
        &impl_a::method1,
        &impl_a::method2,
        &impl_a::method3
    >;

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<impl_a, &impl_a::method1>:
                co_await actor_zeta::dispatch(this, &impl_a::method1, msg);
                break;
            case actor_zeta::msg_id<impl_a, &impl_a::method2>:
                co_await actor_zeta::dispatch(this, &impl_a::method2, msg);
                break;
            case actor_zeta::msg_id<impl_a, &impl_a::method3>:
                co_await actor_zeta::dispatch(this, &impl_a::method3, msg);
                break;
        }
    }

    int call_count() const { return call_count_; }
    int last_value() const { return last_value_; }
    std::string last_text() const { return last_text_; }

private:
    int call_count_;
    int last_value_;
    std::string last_text_;
};

// ============================================================================
// Second implementation (same contract)
// ============================================================================

class impl_b final : public actor_zeta::basic_actor<impl_b> {
public:
    explicit impl_b(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<impl_b>(ptr)
        , call_count_(0) {
    }

    actor_zeta::unique_future<void> method1(int value) {
        ++call_count_;
        values_.push_back(value);
        co_return;
    }

    actor_zeta::unique_future<void> method2(std::string text) {
        ++call_count_;
        texts_.push_back(text);
        co_return;
    }

    actor_zeta::unique_future<int> method3() {
        ++call_count_;
        co_return 100;
    }

    using dispatch_traits = actor_zeta::implements<
        test_contract,
        &impl_b::method1,
        &impl_b::method2,
        &impl_b::method3
    >;

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<impl_b, &impl_b::method1>:
                co_await actor_zeta::dispatch(this, &impl_b::method1, msg);
                break;
            case actor_zeta::msg_id<impl_b, &impl_b::method2>:
                co_await actor_zeta::dispatch(this, &impl_b::method2, msg);
                break;
            case actor_zeta::msg_id<impl_b, &impl_b::method3>:
                co_await actor_zeta::dispatch(this, &impl_b::method3, msg);
                break;
        }
    }

    int call_count() const { return call_count_; }
    const std::vector<int>& values() const { return values_; }
    const std::vector<std::string>& texts() const { return texts_; }

private:
    int call_count_;
    std::vector<int> values_;
    std::vector<std::string> texts_;
};

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("implements - msg_id equality across implementations") {
    // Key test: msg_id should be the same for all implementations of the same contract

    constexpr auto id_a1 = actor_zeta::msg_id<impl_a, &impl_a::method1>;
    constexpr auto id_a2 = actor_zeta::msg_id<impl_a, &impl_a::method2>;
    constexpr auto id_a3 = actor_zeta::msg_id<impl_a, &impl_a::method3>;

    constexpr auto id_b1 = actor_zeta::msg_id<impl_b, &impl_b::method1>;
    constexpr auto id_b2 = actor_zeta::msg_id<impl_b, &impl_b::method2>;
    constexpr auto id_b3 = actor_zeta::msg_id<impl_b, &impl_b::method3>;

    constexpr auto id_c1 = actor_zeta::msg_id<test_contract, &test_contract::method1>;
    constexpr auto id_c2 = actor_zeta::msg_id<test_contract, &test_contract::method2>;
    constexpr auto id_c3 = actor_zeta::msg_id<test_contract, &test_contract::method3>;

    // All three should be equal!
    REQUIRE(id_a1 == id_b1);
    REQUIRE(id_a1 == id_c1);

    REQUIRE(id_a2 == id_b2);
    REQUIRE(id_a2 == id_c2);

    REQUIRE(id_a3 == id_b3);
    REQUIRE(id_a3 == id_c3);
}

TEST_CASE("implements - contract_type is accessible") {
    using contract_from_a = typename impl_a::dispatch_traits::contract_type;
    using contract_from_b = typename impl_b::dispatch_traits::contract_type;

    static_assert(std::is_same_v<contract_from_a, test_contract>);
    static_assert(std::is_same_v<contract_from_b, test_contract>);

    REQUIRE(true);  // Static asserts passed
}

TEST_CASE("implements - send and dispatch work correctly") {
    auto* resource = std::pmr::get_default_resource();

    auto actor_a = actor_zeta::spawn<impl_a>(resource);
    auto actor_b = actor_zeta::spawn<impl_b>(resource);

    // Send to impl_a
    auto [needs_sched_a, future_a] = actor_zeta::send(
            actor_a.get(),
            &impl_a::method1,
        42
    );

    actor_a->resume(10);
    REQUIRE(actor_a->call_count() == 1);
    REQUIRE(actor_a->last_value() == 42);

    // Send to impl_b
    auto [needs_sched_b, future_b] = actor_zeta::send(
            actor_b.get(),
            &impl_b::method1,
        100
    );

    actor_b->resume(10);
    REQUIRE(actor_b->call_count() == 1);
    REQUIRE(actor_b->values().size() == 1);
    REQUIRE(actor_b->values()[0] == 100);
}

TEST_CASE("implements - methods list size is correct") {
    // Check that methods list is correctly built
    static_assert(actor_zeta::type_traits::type_list_size_v<typename impl_a::dispatch_traits::methods> == 3);
    static_assert(actor_zeta::type_traits::type_list_size_v<typename impl_b::dispatch_traits::methods> == 3);

    REQUIRE(true);  // Static asserts passed
}