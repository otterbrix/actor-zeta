#pragma once

// Shared awaiter machinery for actor-zeta coroutine promise types.
//
// The lock-free CAS-based continuation handshake (the `continuation_` compare/exchange
// that was fixed for a cross-thread race in PR #182) used to be DUPLICATED across
// `unique_future::promise_type_base` (future.hpp) and `behavior_t::promise_type`
// (behavior_t.hpp). Adding `task::promise_type` would have created a THIRD copy of
// race-prone lock-free code. This header is the single source of truth: all three promise
// types consume `future_awaiter_mixin<Derived>` (a CRTP mixin) so any future fix to the
// awaiter lands exactly once.
//
// This file is a PURE refactor of the previously-duplicated code: the await behavior is
// byte-for-byte identical. The only unification is that `behavior_t`'s awaiters used to
// clear their awaited-chain via four raw `std::atomic<...>**` double-pointers, whereas the
// unique_future awaiters called `promise_->clear_awaited_chain()`. Because `behavior_t` is
// always the chain root, the manual 4-pointer clear is functionally identical to a 1-level
// `clear_awaited_chain()` (clear own awaited_* then propagate nullptr to the immediate
// parent, which for the root is the same pointers). All awaiters now use the
// `promise_->clear_awaited_chain()` form.

#include <atomic>
#include <cassert>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/state_flags.hpp>

namespace actor_zeta {

    // Forward declarations (full definitions not required to define the mixin templates).
    template<typename T>
    class unique_future;
    template<typename T>
    class generator;

    namespace detail {
        template<typename T>
        struct next_awaiter;

        // --- Shared lock-free CAS suspend (PR #182 fix lives here, once) ---
        // Returns the coroutine to resume next (symmetric transfer), or noop_coroutine() if
        // the producer will resume us later.
        //
        // StateT is shared_state<U>. The behavior is byte-for-byte identical to the previous
        // duplicated copies in future.hpp / behavior_t.hpp.
        template<typename StateT>
        inline detail::coroutine_handle<>
        future_await_suspend_cas(StateT* state, detail::coroutine_handle<> h) noexcept {
            // CAS for setting continuation. This allows detecting double-await (programmer error).
            detail::coroutine_handle<> expected = nullptr;
            if (state->continuation_.compare_exchange_strong(
                    expected, h,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                // CAS successful - we set continuation. Now check: maybe result is already ready?
                if (state->flags_.load(std::memory_order_acquire)
                        & detail::state_flags::result_set) {
                    // Result is ready! Try to take continuation back.
                    auto cont = state->continuation_.exchange(
                        nullptr, std::memory_order_acquire);
                    if (cont) {
                        // We took it - resume ourselves.
                        return cont;
                    }
                    // Producer already took it - they will resume us.
                    return detail::noop_coroutine();
                }
                // Result not ready - wait, producer will resume us.
                return detail::noop_coroutine();
            } else {
                // CAS failed - someone already set continuation.
                // For single-consumer this is a programmer error.
                assert(false && "double co_await on unique_future is undefined behavior");
                return h;  // resume ourselves
            }
        }

        // --- CRTP mixin: awaited-chain tracking + await_transform overloads ---
        //
        // Derived must be the final promise type. The mixin provides:
        //   * the awaited_flags_ / awaited_continuation_ / propagated_to_* / outer_* fields
        //     (used by the actor scheduler's spinning/resume mechanism),
        //   * propagate_awaited_state() / update_propagated_outer() / clear_awaited_chain(),
        //   * await_transform(unique_future<U>&&),
        //   * await_transform(std::pair<bool, unique_future<U>>&&) (incl. void),
        //   * await_transform(generator<U>&),
        //   * the constrained generic passthrough await_transform (foreign awaitables).
        template<typename Derived>
        struct future_awaiter_mixin {
            // Track deepest awaited future for spinning mechanism (propagated through chain).
            std::atomic<std::uint8_t>* awaited_flags_ = nullptr;
            std::atomic<detail::coroutine_handle<>>* awaited_continuation_ = nullptr;

            // Track where our awaited state was propagated to (outer promise's pointers).
            // Used to update outer's copy when we change awaited state, and clear when freeing.
            std::atomic<std::uint8_t>** propagated_to_flags_ = nullptr;
            std::atomic<detail::coroutine_handle<>>** propagated_to_cont_ = nullptr;

            // Type-erased pointer to outer promise for recursive chain clearing.
            // This allows clear_awaited_chain() to walk up to the root and clear all levels.
            // For a root promise (e.g. behavior_t), this is nullptr.
            void* outer_promise_raw_ = nullptr;
            // Function to call update_propagated_outer() on the type-erased outer promise.
            void (*outer_update_fn_)(void*) = nullptr;

            Derived* self() noexcept { return static_cast<Derived*>(this); }

            // === await_transform: unique_future<U>&& ===
            // CRITICAL: The awaiter must OWN the future to prevent premature destruction.
            // If we just extract the state pointer, the temporary unique_future is destroyed
            // right after await_transform returns, setting future_released flag, which causes
            // final_suspend to not resume the waiter (treats as cancelled).
            template<typename U>
            auto await_transform(unique_future<U>&& future) noexcept {
                propagate_awaited_state(future);

                struct owning_awaiter {
                    unique_future<U> owned_;
                    future_awaiter_mixin* promise_;

                    bool await_ready() const noexcept {
                        return owned_.internal_state()->has_result();
                    }

                    detail::coroutine_handle<> await_suspend(detail::coroutine_handle<> h) noexcept {
                        return detail::future_await_suspend_cas(owned_.internal_state(), h);
                    }

                    auto await_resume() {
                        // Clear the entire awaited chain BEFORE freeing the state.
                        promise_->clear_awaited_chain();

                        auto* state = owned_.internal_state();
                        assert(!state->has_error() && "future completed with error");
                        if constexpr (std::is_void_v<U>) {
                            state->take_value();
                        } else {
                            return state->take_value();
                        }
                    }
                };
                return owning_awaiter{std::move(future), this};
            }

            // === await_transform: pair<bool, unique_future<U>>&& from send() ===
            // CRITICAL: Must own the future to prevent premature destruction (see above).
            template<typename U>
            auto await_transform(std::pair<bool, unique_future<U>>&& p) noexcept {
                propagate_awaited_state(p.second);

                struct owning_pair_awaiter {
                    bool needs_sched_;
                    unique_future<U> owned_;
                    future_awaiter_mixin* promise_;

                    bool await_ready() const noexcept {
                        return owned_.internal_state()->has_result();
                    }

                    detail::coroutine_handle<> await_suspend(detail::coroutine_handle<> h) noexcept {
                        return detail::future_await_suspend_cas(owned_.internal_state(), h);
                    }

                    auto await_resume() {
                        // Clear the entire awaited chain BEFORE freeing the state.
                        promise_->clear_awaited_chain();

                        auto* state = owned_.internal_state();
                        assert(!state->has_error() && "future completed with error");
                        if constexpr (std::is_void_v<U>) {
                            state->take_value();
                            return needs_sched_;
                        } else {
                            return std::make_pair(needs_sched_, state->take_value());
                        }
                    }
                };
                return owning_pair_awaiter{p.first, std::move(p.second), this};
            }

            // === await_transform: generator<U>& ===
            template<typename U>
            auto await_transform(generator<U>& gen) noexcept {
                return detail::next_awaiter<U>{gen.internal_state()};
            }

            // NOTE: there is intentionally NO generic foreign-awaitable passthrough
            // await_transform — actor-zeta coroutines only co_await actor-zeta awaitables
            // (see the note at the end of this file).

            // === Awaited-chain propagation (spinning mechanism) ===

            // Propagate deepest awaited state from inner coroutine.
            template<typename U>
            void propagate_awaited_state(unique_future<U>& future) noexcept {
                // If result is already set, coroutine may have been destroyed via final_suspend.
                // No need to track awaited state — await_ready() will return true.
                if (future.internal_state()->has_result()) {
                    awaited_flags_ = nullptr;
                    awaited_continuation_ = nullptr;
                    update_propagated_outer();
                    return;
                }

                auto inner_handle = future.coroutine_handle();
                if (inner_handle) {
                    // Method coroutine — check if it has deeper awaited state.
                    auto& inner_promise = inner_handle.promise();

                    // Set up back-reference for direct field updates.
                    inner_promise.propagated_to_flags_ = &awaited_flags_;
                    inner_promise.propagated_to_cont_ = &awaited_continuation_;

                    // Set up type-erased outer promise pointer for recursive chain clearing.
                    inner_promise.outer_promise_raw_ = this;
                    inner_promise.outer_update_fn_ = &call_update_propagated_outer;

                    if (inner_promise.awaited_flags_) {
                        // Inner coroutine is waiting for something deeper — propagate.
                        awaited_flags_ = inner_promise.awaited_flags_;
                        awaited_continuation_ = inner_promise.awaited_continuation_;
                    } else {
                        // Inner coroutine not waiting — this future is the deepest level.
                        awaited_flags_ = &future.internal_state()->flags_;
                        awaited_continuation_ = &future.internal_state()->continuation_;
                    }
                } else {
                    // Cross-actor future (from promise.get_future()) — this is the deepest level.
                    awaited_flags_ = &future.internal_state()->flags_;
                    awaited_continuation_ = &future.internal_state()->continuation_;
                }

                update_propagated_outer();
            }

            // Static helper to call update_propagated_outer() on a type-erased mixin pointer.
            static void call_update_propagated_outer(void* promise) noexcept {
                static_cast<future_awaiter_mixin*>(promise)->update_propagated_outer();
            }

            // Update outer promise's copy of our awaited state, recursively up to the root.
            void update_propagated_outer() noexcept {
                if (propagated_to_flags_) {
                    *propagated_to_flags_ = awaited_flags_;
                }
                if (propagated_to_cont_) {
                    *propagated_to_cont_ = awaited_continuation_;
                }
                if (outer_update_fn_ && outer_promise_raw_) {
                    outer_update_fn_(outer_promise_raw_);
                }
            }

            // Clear the awaited chain when an await completes.
            // Clears our own awaited state and propagates nullptr up the chain.
            // Called by awaiters in await_resume BEFORE the awaited future is destroyed.
            void clear_awaited_chain() noexcept {
                awaited_flags_ = nullptr;
                awaited_continuation_ = nullptr;
                update_propagated_outer();
            }
        };

        // NOTE: there is intentionally NO generic foreign-awaitable passthrough await_transform.
        // An actor IS a coroutine (behavior_t); actor coroutines (and the unique_future method
        // coroutines they co_await) only ever co_await actor-zeta awaitables (unique_future /
        // pair / generator), driven by the sharing_scheduler. They never co_await a foreign
        // (e.g. Asio) awaitable — that would resume the actor off its scheduler thread. External
        // event loops integrate the OTHER way: a foreign coroutine co_awaits OUR unique_future
        // via unique_future::operator co_await (+ a bridge on the foreign side). So all promise
        // types consume the single future_awaiter_mixin above, with only the specific overloads.

    } // namespace detail
} // namespace actor_zeta
