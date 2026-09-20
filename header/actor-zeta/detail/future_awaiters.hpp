#pragma once

// Shared awaiter machinery for actor-zeta coroutine promise types.
//
// The lock-free `continuation_` compare/exchange is race-prone code that both
// `unique_future::promise_type_base` (future.hpp) and `behavior_t::promise_type`
// (behavior_t.hpp) need. This header is the single source of truth: every promise type
// consumes `future_awaiter_mixin<Derived>` (a CRTP mixin), so a fix to the awaiter lands
// exactly once instead of once per copy.

#include <atomic>
#include <cassert>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/result_storage.hpp>
#include <actor-zeta/detail/state_flags.hpp>

namespace actor_zeta {

    // Forward declarations (full definitions not required to define the mixin templates).
    template<typename T>
    class unique_future;

    namespace detail {
        // --- Shared lock-free CAS suspend ---
        // Returns the coroutine to resume next (symmetric transfer), or noop_coroutine() if
        // the producer will resume us later. StateT is shared_state<U>.
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
                    // The producer took our handle. It resumes us only for a
                    // method-coroutine state, via promise_type_base::final_awaiter.
                    // For a promise<T>-backed state (everything send() returns)
                    // completion is flag-only and NOBODY resumes: the consumer's own
                    // driver picks it up in cooperative_actor's Q6 block.
                    return detail::noop_coroutine();
                }
                // Result not ready. Same split as above -- for a promise<T>-backed
                // state we are woken by our own driver polling is_awaited_ready(),
                // not by the producer.
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
        //   * await_transform(std::pair<bool, unique_future<U>>&&) (incl. void).
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
                        // Rethrow at the co_await point, so the exception surfaces where
                        // the value would have. It then reaches THIS coroutine's own
                        // unhandled_exception() and is captured into its state -- which is
                        // how a failure propagates up a chain of awaits.
                        state->rethrow_if_exception();
                        // No exception to rethrow and still an error: there is no value
                        // here and co_await has no channel to report that. Refuse rather
                        // than extract -- see refuse_valueless_extraction(). Cancellation
                        // is observed by polling failed(), not by awaiting.
                        if (state->has_error()) {
                            refuse_valueless_extraction("co_await");
                        }
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
            //
            // Prohibited, and the overload exists only to say so.
            //
            // `co_await send(target, ...)` compiles into a suspension that can never
            // end. send() returns {needs_sched, future}: needs_sched is the obligation
            // to put `target` in a run queue, and NOTHING in the library discharges it.
            // An awaiter could only hand it back from await_resume() -- i.e. AFTER the
            // wait -- and the wait cannot finish until `target` has run. A coroutine has
            // an address_t and no scheduler, so it cannot discharge the obligation
            // itself. The actor then spins in the run queue forever, with no diagnostic.
            //
            // It is not "wrong under some conditions": the bool is delivered past the
            // point where it was needed, always. So this is a compile error rather than
            // an assert -- the shape is decidable from the type alone.
            template<typename U>
            auto await_transform(std::pair<bool, unique_future<U>>&& p) noexcept {
                static_assert(sizeof(U) == 0,
                              "co_await send(...) never completes: the needs_sched half "
                              "of send()'s result is only delivered after the await, and "
                              "the await cannot finish until someone has scheduled the "
                              "target with it. Split it: "
                              "auto [needs_sched, f] = send(target, &T::m, args...); "
                              "if (needs_sched) scheduler->enqueue(target); "
                              "auto r = co_await std::move(f);");
                return detail::suspend_never{};
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
        // coroutines they co_await) only ever co_await actor-zeta awaitables, driven by the
        // sharing_scheduler. A foreign (e.g. Asio) awaitable would resume the actor off its
        // scheduler thread.
        //
        // External event loops integrate by POLLING instead: is_ready(), then failed(), then
        // take_ready() -- see examples/external-drive/. The failed() step is not optional:
        // is_ready() is the promise_released bit, which a promise dying without a value also
        // sets, and take_ready() only ASSERTS the value is present. unique_future also has no
        // operator co_await: a foreign consumer suspended on a promise<T>-backed future would
        // hang forever, because its completion is flag-only and no cooperative_actor drives
        // that consumer.

    } // namespace detail
} // namespace actor_zeta
