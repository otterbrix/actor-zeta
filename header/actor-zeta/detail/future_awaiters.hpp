#pragma once

// The `continuation_` CAS and awaited-chain propagation shared by both promise types, as one CRTP mixin.

#include <atomic>
#include <cassert>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/result_storage.hpp>
#include <actor-zeta/detail/state_flags.hpp>

namespace actor_zeta {

    template<typename T>
    class unique_future;

    namespace detail {
        // Returns the coroutine to resume next, or noop_coroutine() if resumed later. Only a
        // method-coroutine producer resumes us (final_awaiter); a promise<T>-backed state --
        // everything send() returns -- completes flag-only, and our own driver's drain picks us up.
        template<typename StateT>
        inline detail::coroutine_handle<>
        future_await_suspend_cas(StateT* state, detail::coroutine_handle<> h) noexcept {
            detail::coroutine_handle<> expected = nullptr;
            if (state->continuation_.compare_exchange_strong(
                    expected, h,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                // The producer may have finished before we registered.
                if (state->flags_.load(std::memory_order_acquire)
                        & detail::state_flags::result_set) {
                    auto cont = state->continuation_.exchange(
                        nullptr, std::memory_order_acquire);
                    if (cont) {
                        return cont;
                    }
                    return detail::noop_coroutine();
                }
                return detail::noop_coroutine();
            } else {
                assert(false && "double co_await on unique_future is undefined behavior");
                return h;  // resume ourselves
            }
        }

        template<typename Derived>
        struct future_awaiter_mixin {
            // Deepest awaited state (the drain reads it); the outer's copy of it; the outer itself, type-erased.
            std::atomic<std::uint8_t>* awaited_flags_ = nullptr;
            std::atomic<detail::coroutine_handle<>>* awaited_continuation_ = nullptr;

            std::atomic<std::uint8_t>** propagated_to_flags_ = nullptr;
            std::atomic<detail::coroutine_handle<>>** propagated_to_cont_ = nullptr;

            void* outer_promise_raw_ = nullptr;
            void (*outer_update_fn_)(void*) = nullptr;

            Derived* self() noexcept { return static_cast<Derived*>(this); }

            // Must OWN the future: a temporary dying after await_transform would read as a cancelled await.
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
                        // Rethrow here; THIS coroutine's unhandled_exception() captures it, so failures climb the chain.
                        state->rethrow_if_exception();
                        // A bare error has no co_await channel: refuse. Cancellation is observed by polling failed().
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

            // Exists only to reject `co_await send(...)`, which can never complete; the message says why.
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

            template<typename U>
            void propagate_awaited_state(unique_future<U>& future) noexcept {
                if (future.internal_state()->has_result()) {
                    awaited_flags_ = nullptr;
                    awaited_continuation_ = nullptr;
                    update_propagated_outer();
                    return;
                }

                auto inner_handle = future.coroutine_handle();
                if (inner_handle) {
                    auto& inner_promise = inner_handle.promise();

                    inner_promise.propagated_to_flags_ = &awaited_flags_;
                    inner_promise.propagated_to_cont_ = &awaited_continuation_;

                    inner_promise.outer_promise_raw_ = this;
                    inner_promise.outer_update_fn_ = &call_update_propagated_outer;

                    if (inner_promise.awaited_flags_) {
                        awaited_flags_ = inner_promise.awaited_flags_;
                        awaited_continuation_ = inner_promise.awaited_continuation_;
                    } else {
                        awaited_flags_ = &future.internal_state()->flags_;
                        awaited_continuation_ = &future.internal_state()->continuation_;
                    }
                } else {
                    awaited_flags_ = &future.internal_state()->flags_;
                    awaited_continuation_ = &future.internal_state()->continuation_;
                }

                update_propagated_outer();
            }

            static void call_update_propagated_outer(void* promise) noexcept {
                static_cast<future_awaiter_mixin*>(promise)->update_propagated_outer();
            }

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

            void clear_awaited_chain() noexcept {
                awaited_flags_ = nullptr;
                awaited_continuation_ = nullptr;
                update_propagated_outer();
            }
        };

        // Deliberately NO generic foreign-awaitable await_transform (a foreign awaitable would resume
        // the actor off its scheduler thread) and NO operator co_await (a foreign consumer would hang:
        // completion is flag-only). External loops POLL: is_ready(), then failed() -- not optional, as
        // is_ready() is only promise_released, which a valueless death also sets -- then take_ready().

    } // namespace detail
} // namespace actor_zeta
