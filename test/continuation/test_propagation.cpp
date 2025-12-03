#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/intrusive_ptr.hpp>

#include <thread>
#include <chrono>

using namespace actor_zeta::detail;

// Helper: allocate future_state using PMR (matches destroy() deallocation)
template<typename T>
future_state<T>* allocate_future_state(actor_zeta::pmr::memory_resource* resource) {
    void* mem = resource->allocate(sizeof(future_state<T>), alignof(future_state<T>));
    return new (mem) future_state<T>(resource);
}

TEST_CASE("forward_target propagation - basic typed", "[forward_target][propagation]") {
    auto* resource = actor_zeta::pmr::get_default_resource();

    // Create source and target states using PMR allocate (matches destroy() deallocation)
    auto* source_state = allocate_future_state<int>(resource);
    auto* target_state = allocate_future_state<int>(resource);

    // Wrap in intrusive_ptr for RAII (adopt_ref = false to adopt initial ref)
    actor_zeta::intrusive_ptr<future_state_base> source(source_state, false);
    actor_zeta::intrusive_ptr<future_state_base> target(target_state, false);

    SECTION("forward_target set before result") {
        // Set forward_target BEFORE result is ready
        source_state->set_forward_target(target_state);

        REQUIRE(!source_state->is_ready());
        REQUIRE(!target_state->is_ready());

        // Set result in source using new API
        source_state->set_value(42);

        // Both should be ready now
        REQUIRE(source_state->is_ready());
        REQUIRE(target_state->is_ready());

        // Target should have the result (forwarded)
        // New API: get_value() returns T directly
        REQUIRE(target_state->get_value() == 42);
    }

    SECTION("forward_target set after result - no propagation") {
        // Set result FIRST
        source_state->set_value(42);

        REQUIRE(source_state->is_ready());
        REQUIRE(!target_state->is_ready());

        // Set forward_target AFTER result is ready
        // This should assert/fail (forward_target only works for pending futures)
        // We can't test this directly as it will assert
        // Just verify source is ready and target is pending
        REQUIRE(source_state->is_ready());
        REQUIRE(!target_state->is_ready());
    }
}

// ============================================================================
// CRITICAL TEST: Void forward_target propagation
// ============================================================================
TEST_CASE("forward_target propagation - void specialization", "[forward_target][propagation][void][critical]") {
    auto* resource = actor_zeta::pmr::get_default_resource();

    auto* source_state = allocate_future_state<void>(resource);
    auto* target_state = allocate_future_state<void>(resource);

    actor_zeta::intrusive_ptr<future_state_base> source(source_state, false);
    actor_zeta::intrusive_ptr<future_state_base> target(target_state, false);

    SECTION("void forward_target propagation via set_ready()") {
        // Set forward_target BEFORE ready
        source_state->set_forward_target(target_state);

        REQUIRE(!source_state->is_ready());
        REQUIRE(!target_state->is_ready());

        // Set ready (void has no result value)
        source_state->set_ready();

        // Both should be ready now
        REQUIRE(source_state->is_ready());
        REQUIRE(target_state->is_ready());
    }
}

TEST_CASE("forward_target propagation - chain of 3 typed", "[forward_target][propagation][chain]") {
    auto* resource = actor_zeta::pmr::get_default_resource();

    // Create chain: A → B → C
    auto* state_a = allocate_future_state<int>(resource);
    auto* state_b = allocate_future_state<int>(resource);
    auto* state_c = allocate_future_state<int>(resource);

    // Keep all states alive during test with intrusive_ptr (adopt_ref = false)
    actor_zeta::intrusive_ptr<future_state_base> a(state_a, false);
    actor_zeta::intrusive_ptr<future_state_base> b(state_b, false);
    actor_zeta::intrusive_ptr<future_state_base> c(state_c, false);

    // Set forward_targets: A → B → C
    state_a->set_forward_target(state_b);
    state_b->set_forward_target(state_c);

    REQUIRE(!state_a->is_ready());
    REQUIRE(!state_b->is_ready());
    REQUIRE(!state_c->is_ready());

    // Set result in A - should propagate through chain
    state_a->set_value(123);

    // All should be ready now
    REQUIRE(state_a->is_ready());
    REQUIRE(state_b->is_ready());
    REQUIRE(state_c->is_ready());

    // Result is in the LAST element of chain (C)
    REQUIRE(state_c->get_value() == 123);
}

TEST_CASE("forward_target propagation - chain of 3 void", "[forward_target][propagation][chain][void]") {
    auto* resource = actor_zeta::pmr::get_default_resource();

    // Create void chain: A → B → C
    auto* state_a = allocate_future_state<void>(resource);
    auto* state_b = allocate_future_state<void>(resource);
    auto* state_c = allocate_future_state<void>(resource);

    actor_zeta::intrusive_ptr<future_state_base> a(state_a, false);
    actor_zeta::intrusive_ptr<future_state_base> b(state_b, false);
    actor_zeta::intrusive_ptr<future_state_base> c(state_c, false);

    // Set forward_targets: A → B → C
    state_a->set_forward_target(state_b);
    state_b->set_forward_target(state_c);

    REQUIRE(!state_a->is_ready());
    REQUIRE(!state_b->is_ready());
    REQUIRE(!state_c->is_ready());

    // Set ready in A - should propagate through chain
    state_a->set_ready();

    // All should be ready now
    REQUIRE(state_a->is_ready());
    REQUIRE(state_b->is_ready());
    REQUIRE(state_c->is_ready());
}

TEST_CASE("forward_target propagation - no double propagation", "[forward_target][edge-case]") {
    auto* resource = actor_zeta::pmr::get_default_resource();

    auto* source_state = allocate_future_state<int>(resource);
    auto* target_state = allocate_future_state<int>(resource);

    actor_zeta::intrusive_ptr<future_state_base> source(source_state, false);
    actor_zeta::intrusive_ptr<future_state_base> target(target_state, false);

    // Set forward_target
    source_state->set_forward_target(target_state);

    // Set result multiple times (CAS protection!)
    source_state->set_value(42);
    source_state->set_value(999);  // Should be ignored (CAS fails)

    // Both should be ready
    REQUIRE(source_state->is_ready());
    REQUIRE(target_state->is_ready());

    // Target should have 42 (first set), not 999
    REQUIRE(target_state->get_value() == 42);
}

TEST_CASE("forward_target propagation - void no double propagation", "[forward_target][edge-case][void]") {
    auto* resource = actor_zeta::pmr::get_default_resource();

    auto* source_state = allocate_future_state<void>(resource);
    auto* target_state = allocate_future_state<void>(resource);

    actor_zeta::intrusive_ptr<future_state_base> source(source_state, false);
    actor_zeta::intrusive_ptr<future_state_base> target(target_state, false);

    // Set forward_target
    source_state->set_forward_target(target_state);

    // Set ready multiple times (transition should fail after first)
    source_state->set_ready();
    source_state->set_ready();  // Should be ignored (transition fails)

    // Both should be ready (only once)
    REQUIRE(source_state->is_ready());
    REQUIRE(target_state->is_ready());
}

TEST_CASE("forward_target propagation - cancelled source", "[forward_target][edge-case]") {
    auto* resource = actor_zeta::pmr::get_default_resource();

    auto* source_state = allocate_future_state<int>(resource);
    auto* target_state = allocate_future_state<int>(resource);

    actor_zeta::intrusive_ptr<future_state_base> source(source_state, false);
    actor_zeta::intrusive_ptr<future_state_base> target(target_state, false);

    // Set forward_target
    source_state->set_forward_target(target_state);

    // Cancel source
    source_state->set_state(future_state_enum::cancelled);

    REQUIRE(source_state->is_cancelled());
    REQUIRE(!target_state->is_ready());

    // Try to set result - should be ignored (source is cancelled)
    source_state->set_value(42);

    // Target should remain pending (source was cancelled)
    REQUIRE(!target_state->is_ready());
}