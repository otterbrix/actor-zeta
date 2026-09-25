#pragma once

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <utility>

#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/coro_frame_header.hpp>
#include <actor-zeta/detail/future_awaiters.hpp>
#include <actor-zeta/detail/state_flags.hpp>
#include <actor-zeta/detail/type_traits.hpp>

namespace actor_zeta {

    template<typename T>
    class unique_future;

    /// The behavior() coroutine, one per message: cooperative_actor's loop (run()) keeps it as a
    /// local while it is suspended; an actor_mixin that calls behavior() itself owns what it gets.
    struct behavior_t {
        // Awaiting comes from future_awaiter_mixin, as for unique_future; behavior_t is always the chain ROOT.
        struct promise_type : detail::future_awaiter_mixin<promise_type> {
            behavior_t get_return_object() noexcept {
                return behavior_t{detail::coroutine_handle<promise_type>::from_promise(*this)};
            }

            // Load-bearing, not style: suspend_never means the body has reached its first co_await
            // (or finished) by the time `self()->behavior(msg)` returns in the loop, so is_busy() tells
            // the two apart; suspend_always would read as finished, and the loop would drop a frame
            // that never ran.
            detail::suspend_never initial_suspend() noexcept { return {}; }

            // Stay suspended; ~behavior_t() destroys the frame, which never runs elsewhere (completion is flag-only).
            detail::suspend_always final_suspend() noexcept { return {}; }

            void return_void() noexcept {}

            // Reachable only from behavior() itself (dispatch() catches a method's throw). Discarded:
            // the chain root has no shared_state and no consumer to rethrow at. Reported; the loop
            // takes the next message.
            void unhandled_exception() noexcept {
#ifdef __cpp_exceptions
                // Rethrown locally only to read what().
                try {
                    throw;
                } catch (const std::exception& e) {
                    std::fprintf(stderr,
                                 "actor-zeta: an exception escaped behavior() and was "
                                 "discarded: %s\n"
                                 "  behavior() is the root of the await chain -- its result "
                                 "is read by nobody, so there is\n"
                                 "  nowhere to deliver this. Put the work in a dispatched "
                                 "method instead: a throw there\n"
                                 "  reaches the caller's future.\n",
                                 e.what());
                } catch (...) {
                    std::fprintf(stderr,
                                 "actor-zeta: a non-std::exception escaped behavior() and "
                                 "was discarded.\n"
                                 "  Put the work in a dispatched method instead: a throw "
                                 "there reaches the caller's future.\n");
                }
#else
                // No catch wrapper without exceptions: reaching this is a toolchain fault, not user code.
                assert(false && "unhandled_exception() with -fno-exceptions");
                std::terminate();
#endif
            }

            // The await_transform overloads are inherited; there is deliberately NO generic
            // foreign-awaitable passthrough (see future_awaiters.hpp). That absence is
            // load-bearing: await_transform is a MEMBER, so [expr.await]/3.2 routes EVERY
            // co_await through it and every suspension publishes awaited_flags_ -- which is what
            // keeps a suspended behavior is_busy(), so the loop keeps it, and the turn, instead of
            // taking it for finished and destroying a live frame. An overload that skipped the
            // publication, or a yield_value, would break that silently;
            // test/foreign-awaitable-prohibited checks.

            // The frame comes from the actor's memory resource, as the loop's frame does. behavior() is
            // the actor's member, so the actor comes first -- unless this is not an actor's behavior()
            // at all, or GCC dropped `this` (it does for out-of-line methods).
            template<typename Self, typename... Args>
            static void* operator new(std::size_t size, const Self& self, const Args&...) {
                static_assert(detail::has_resource_method<const Self>,
                              "behavior() must be an actor member function defined inline: its frame "
                              "comes from the actor's resource() (GCC does not pass 'this' for "
                              "out-of-line methods)");
                if constexpr (detail::has_resource_method<const Self>) {
                    return detail::allocate_coro_frame(self.resource(), size);
                } else {
                    std::abort(); // unreachable: the static_assert stops the build
                }
            }

            // No arguments at all: not a member function, so nothing can supply the resource. Chosen
            // only then -- the overload above is more specialized. The empty pack is there because a
            // template allocation function needs a second parameter.
            template<typename... None>
            static void* operator new(std::size_t, const None&...) {
                static_assert(sizeof...(None) != 0,
                              "behavior() must be an actor member function defined inline: its frame "
                              "comes from the actor's resource()");
                std::abort();
            }

            static void operator delete(void* ptr, std::size_t size) noexcept {
                detail::deallocate_coro_frame(ptr, size);
            }

            static void operator delete(void* ptr) noexcept {
                detail::deallocate_coro_frame_unsized(ptr);
            }
        };

        detail::coroutine_handle<promise_type> handle_;

        explicit behavior_t(detail::coroutine_handle<promise_type> h) noexcept
            : handle_(h) {}

        behavior_t(behavior_t&& o) noexcept
            : handle_(std::exchange(o.handle_, {})) {}

        // Unconditional destroy is safe: the frame is done or suspended, never running elsewhere.
        ~behavior_t() {
            if (handle_) {
                handle_.destroy();
            }
        }

        behavior_t(const behavior_t&) = delete;
        behavior_t& operator=(const behavior_t&) = delete;

        /// Suspended on a co_await.
        [[nodiscard]] bool is_busy() const noexcept {
            return handle_ && !handle_.done() && handle_.promise().awaited_flags_ != nullptr;
        }

        /// The deepest awaited continuation once its result is ready, or null. Gates on
        /// promise_released alone: the result may be error_set only, and such a state must still
        /// drain; the refusal belongs at extraction (owning_awaiter::await_resume).
        [[nodiscard]] detail::coroutine_handle<> take_awaited_continuation() noexcept {
            if (!is_busy()) {
                return nullptr;
            }
            auto& root = handle_.promise();
            if (!(root.awaited_flags_->load(std::memory_order_acquire) & detail::state_flags::promise_released)) {
                return nullptr;
            }
            return root.awaited_continuation_->exchange(nullptr, std::memory_order_acq_rel);
        }
    };

} // namespace actor_zeta
