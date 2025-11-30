#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/rtt.hpp>
#include <actor-zeta/detail/intrusive_ptr.hpp>

#include <thread>
#include <chrono>

using namespace actor_zeta::detail;

TEST_CASE("forward_target propagation - basic typed", "[forward_target][propagation]") {
    auto* resource = actor_zeta::pmr::get_default_resource();

    // Create source and target states
    auto* source_state = new future_state<int>(resource);
    auto* target_state = new future_state<int>(resource);

    // Wrap in intrusive_ptr for RAII
    actor_zeta::intrusive_ptr<future_state_base> source(source_state);
    actor_zeta::intrusive_ptr<future_state_base> target(target_state);

    SECTION("forward_target set before result") {
        // Set forward_target BEFORE result is ready
        source_state->set_forward_target(target_state);

        REQUIRE(!source_state->is_ready());
        REQUIRE(!target_state->is_ready());

        // Set result in source
        rtt value(resource, 42);
        source_state->set_result_rtt(std::move(value));

        // Both should be ready now
        REQUIRE(source_state->is_ready());
        REQUIRE(target_state->is_ready());

        // Target should have the result (forwarded)
        auto& target_result = target_state->result();
        REQUIRE(target_result.get<int>(0) == 42);
    }

    SECTION("forward_target set after result - no propagation") {
        // Set result FIRST
        rtt value(resource, 42);
        source_state->set_result_rtt(std::move(value));

        REQUIRE(source_state->is_ready());
        REQUIRE(!target_state->is_ready());

        // Set forward_target AFTER result is ready
        // This should NOT propagate (forward_target only works for pending futures)
        source_state->set_forward_target(target_state);

        // Source is ready, but target should remain pending
        REQUIRE(source_state->is_ready());
        REQUIRE(!target_state->is_ready());
        // This is expected behavior - forward_target is for async propagation
    }
}

// ============================================================================
// CRITICAL TEST: Void forward_target propagation
// This tests the fix for Issue #1 where async void coroutines were not
// propagating their completion status to the caller's future
// ============================================================================
TEST_CASE("forward_target propagation - void specialization", "[forward_target][propagation][void][critical]") {
    auto* resource = actor_zeta::pmr::get_default_resource();

    auto* source_state = new future_state<void>(resource);
    auto* target_state = new future_state<void>(resource);

    actor_zeta::intrusive_ptr<future_state_base> source(source_state);
    actor_zeta::intrusive_ptr<future_state_base> target(target_state);

    SECTION("void forward_target propagation via set_ready()") {
        // This is the CRITICAL test for Issue #1
        // set_ready() must propagate to forward_target_

        // Set forward_target BEFORE ready
        source_state->set_forward_target(target_state);

        REQUIRE(!source_state->is_ready());
        REQUIRE(!target_state->is_ready());

        // Set ready (void has no result value)
        // This simulates: return_void() → set_ready()
        source_state->set_ready();

        // CRITICAL: Both should be ready now!
        // Before fix: target_state remained pending (BUG!)
        // After fix: target_state is ready (CORRECT!)
        REQUIRE(source_state->is_ready());
        REQUIRE(target_state->is_ready());  // <-- This was failing before fix!
    }

    SECTION("void forward_target propagation via set_result_rtt()") {
        // set_result_rtt() for void delegates to set_ready()
        // This also must propagate to forward_target_

        source_state->set_forward_target(target_state);

        REQUIRE(!source_state->is_ready());
        REQUIRE(!target_state->is_ready());

        // set_result_rtt() for void ignores the value and calls set_ready()
        rtt empty_value(resource);
        source_state->set_result_rtt(std::move(empty_value));

        REQUIRE(source_state->is_ready());
        REQUIRE(target_state->is_ready());
    }
}

TEST_CASE("forward_target propagation - chain of 3 typed", "[forward_target][propagation][chain]") {
    auto* resource = actor_zeta::pmr::get_default_resource();

    // Create chain: A → B → C
    auto* state_a = new future_state<int>(resource);
    auto* state_b = new future_state<int>(resource);
    auto* state_c = new future_state<int>(resource);

    // Keep all states alive during test with intrusive_ptr
    actor_zeta::intrusive_ptr<future_state_base> a(state_a);
    actor_zeta::intrusive_ptr<future_state_base> b(state_b);
    actor_zeta::intrusive_ptr<future_state_base> c(state_c);

    // Set forward_targets: A → B → C
    state_a->set_forward_target(state_b);
    state_b->set_forward_target(state_c);

    REQUIRE(!state_a->is_ready());
    REQUIRE(!state_b->is_ready());
    REQUIRE(!state_c->is_ready());

    // Set result in A - should propagate through chain
    rtt value(resource, 123);
    state_a->set_result_rtt(std::move(value));

    // All should be ready now
    REQUIRE(state_a->is_ready());
    REQUIRE(state_b->is_ready());
    REQUIRE(state_c->is_ready());

    // Result is in the LAST element of chain (C)
    // Intermediate states (A, B) forwarded their results
    REQUIRE(state_c->result().get<int>(0) == 123);
}

TEST_CASE("forward_target propagation - chain of 3 void", "[forward_target][propagation][chain][void]") {
    auto* resource = actor_zeta::pmr::get_default_resource();

    // Create void chain: A → B → C
    auto* state_a = new future_state<void>(resource);
    auto* state_b = new future_state<void>(resource);
    auto* state_c = new future_state<void>(resource);

    actor_zeta::intrusive_ptr<future_state_base> a(state_a);
    actor_zeta::intrusive_ptr<future_state_base> b(state_b);
    actor_zeta::intrusive_ptr<future_state_base> c(state_c);

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

    auto* source_state = new future_state<int>(resource);
    auto* target_state = new future_state<int>(resource);

    actor_zeta::intrusive_ptr<future_state_base> source(source_state);
    actor_zeta::intrusive_ptr<future_state_base> target(target_state);

    // Set forward_target
    source_state->set_forward_target(target_state);

    // Set result multiple times (CAS protection!)
    rtt value1(resource, 42);
    source_state->set_result_rtt(std::move(value1));

    rtt value2(resource, 999);
    source_state->set_result_rtt(std::move(value2));  // Should be ignored (CAS fails)

    // Both should be ready
    REQUIRE(source_state->is_ready());
    REQUIRE(target_state->is_ready());

    // Target should have 42 (first set), not 999
    REQUIRE(target_state->result().get<int>(0) == 42);
}

TEST_CASE("forward_target propagation - void no double propagation", "[forward_target][edge-case][void]") {
    auto* resource = actor_zeta::pmr::get_default_resource();

    auto* source_state = new future_state<void>(resource);
    auto* target_state = new future_state<void>(resource);

    actor_zeta::intrusive_ptr<future_state_base> source(source_state);
    actor_zeta::intrusive_ptr<future_state_base> target(target_state);

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

    auto* source_state = new future_state<int>(resource);
    auto* target_state = new future_state<int>(resource);

    actor_zeta::intrusive_ptr<future_state_base> source(source_state);
    actor_zeta::intrusive_ptr<future_state_base> target(target_state);

    // Set forward_target
    source_state->set_forward_target(target_state);

    // Cancel source
    source_state->set_state(future_state_enum::cancelled);

    REQUIRE(source_state->is_cancelled());
    REQUIRE(!target_state->is_ready());

    // Try to set result - should be ignored (source is cancelled)
    rtt value(resource, 42);
    source_state->set_result_rtt(std::move(value));

    // Target should remain pending (source was cancelled)
    REQUIRE(!target_state->is_ready());
}