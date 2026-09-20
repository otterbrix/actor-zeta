#pragma once

#include <cassert>
#include <cstdio>
#include <exception>
#include <memory_resource>
#include <utility>

#include <actor-zeta/config.hpp>
#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/coro_frame_header.hpp>
#include <actor-zeta/detail/future_awaiters.hpp>
#include <actor-zeta/detail/state_flags.hpp>
#include <actor-zeta/detail/type_traits.hpp>

namespace actor_zeta {

    template<typename T>
    class unique_future;

    /// The behavior() coroutine: one per actor, held in cooperative_actor::current_behavior_.
    struct behavior_t {
        // Awaiting comes from future_awaiter_mixin, as for unique_future; behavior_t is always the chain ROOT.
        struct promise_type : detail::future_awaiter_mixin<promise_type> {
            std::pmr::memory_resource* resource_ = nullptr;

            behavior_t get_return_object() noexcept {
                return behavior_t{detail::coroutine_handle<promise_type>::from_promise(*this)};
            }

            // Load-bearing, not style: suspend_never means the body has reached its first co_await
            // (or finished) by the time `current_behavior_ = self()->behavior(msg)` returns, so no
            // behavior is ever live-but-not-started; suspend_always could hand park() a live frame (lost wakeup).
            detail::suspend_never initial_suspend() noexcept { return {}; }

            // Stay suspended; ~behavior_t() destroys the frame, which never runs elsewhere (completion is flag-only).
            auto final_suspend() noexcept {
                struct final_awaiter {
                    bool await_ready() const noexcept { return false; }
                    void await_suspend(detail::coroutine_handle<promise_type>) const noexcept {
                    }
                    void await_resume() const noexcept {}
                };
                return final_awaiter{};
            }

            void return_void() noexcept {}

            // Reachable only from behavior() itself (dispatch() catches a method's throw). Discarded:
            // the chain root has no shared_state and no consumer to rethrow at. Reported, then parks.
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
            // keeps a suspended behavior is_busy() and out of park(). A third overload or a
            // yield_value would skip that silently; test/foreign-awaitable-prohibited checks.

            promise_type() noexcept
                : resource_(nullptr) {}

            template<typename First, typename... Args>
            promise_type(First&& first, Args&&...) noexcept
                : resource_(extract_resource_or_null(std::forward<std::remove_reference_t<First>>(first))) {}

            template<typename... Args>
            static void* operator new(std::size_t size, const Args&... args) {
                auto* res = extract_resource_or_abort(args...);
                return detail::allocate_coro_frame(res, size);
            }

            template<typename... Args>
            static void operator delete(void* ptr, std::size_t size, const Args&...) noexcept {
                detail::deallocate_coro_frame(ptr, size);
            }

            static void operator delete(void* ptr, std::size_t size) noexcept {
                detail::deallocate_coro_frame(ptr, size);
            }

            static void operator delete(void* ptr) noexcept {
                detail::deallocate_coro_frame_unsized(ptr);
            }

        private:
            template<typename U>
            static std::pmr::memory_resource* try_get_resource(U* ptr) noexcept {
                if constexpr (detail::has_resource_method<U>) {
                    return ptr->resource();
                } else {
                    return nullptr;
                }
            }

            template<typename U>
            static std::pmr::memory_resource* extract_resource_impl(U&& arg) noexcept {
                using decayed = std::decay_t<U>;
                if constexpr (std::is_pointer_v<decayed>) {
                    using ptr_type = std::remove_reference_t<U>;
                    return try_get_resource(static_cast<ptr_type>(arg));
                } else {
                    return try_get_resource(&arg);
                }
            }

            static std::pmr::memory_resource* extract_resource_impl(std::pmr::memory_resource* res) noexcept {
                return res;
            }

            template<typename U>
            static constexpr bool supplies_resource() noexcept {
                using decayed = std::decay_t<U>;
                if constexpr (std::is_same_v<decayed, std::pmr::memory_resource*>) {
                    return true;
                } else if constexpr (std::is_pointer_v<decayed>) {
                    return detail::has_resource_method<
                        std::remove_pointer_t<std::remove_reference_t<U>>>;
                } else {
                    return detail::has_resource_method<std::remove_reference_t<U>>;
                }
            }

            static std::pmr::memory_resource* extract_resource_from_args() noexcept {
                return nullptr;
            }

            template<typename First, typename... Rest>
            static std::pmr::memory_resource* extract_resource_from_args(First&& first, Rest&&... rest) noexcept {
                auto res = extract_resource_impl(std::forward<First>(first));
                if (res != nullptr)
                    return res;
                if constexpr (sizeof...(Rest) > 0) {
                    return extract_resource_from_args(std::forward<Rest>(rest)...);
                }
                return nullptr;
            }

            template<typename First, typename... Rest>
            static std::pmr::memory_resource* extract_resource_or_null(First&& first, Rest&&... rest) noexcept {
                return extract_resource_from_args(std::forward<First>(first), std::forward<Rest>(rest)...);
            }

            // Zero arguments: nothing can carry a resource -- a property of the SIGNATURE.
            template<typename Dependent = promise_type>
            [[noreturn]] static std::pmr::memory_resource* extract_resource_or_abort() noexcept {
                static_assert(sizeof(Dependent) == 0,
                              "behavior() must be an actor member function defined inline "
                              "(GCC does not pass 'this' for out-of-line methods)");
                std::abort();
            }

            template<typename First, typename... Rest>
            RETURNS_NONNULL static std::pmr::memory_resource* extract_resource_or_abort(First&& first, Rest&&... rest) noexcept {
                static_assert((supplies_resource<First>() || ... || supplies_resource<Rest>()),
                              "no argument of behavior() can supply a memory resource -- "
                              "it must be an actor member function so `this` is in the pack");
                auto* res = extract_resource_from_args(std::forward<First>(first), std::forward<Rest>(rest)...);
                assert(res != nullptr && "resource() returned null");
                if (!res) {
                    std::abort();
                }
                return res;
            }
        };

        detail::coroutine_handle<promise_type> handle_;

        behavior_t() noexcept
            : handle_{} {}

        explicit behavior_t(detail::coroutine_handle<promise_type> h) noexcept
            : handle_(h) {}

        behavior_t(behavior_t&& o) noexcept
            : handle_(std::exchange(o.handle_, {})) {}

        // Unconditional destroy is safe: the frame is done or suspended, never running elsewhere.
        behavior_t& operator=(behavior_t&& o) noexcept {
            if (this != &o) {
                if (handle_) {
                    handle_.destroy();
                }
                handle_ = std::exchange(o.handle_, {});
            }
            return *this;
        }

        ~behavior_t() {
            if (handle_) {
                handle_.destroy();
            }
        }

        behavior_t(const behavior_t&) = delete;
        behavior_t& operator=(const behavior_t&) = delete;

        [[nodiscard]] bool done() const noexcept {
            return !handle_ || handle_.done();
        }

        explicit operator bool() const noexcept {
            return handle_ != nullptr;
        }

        /// Suspended on a co_await.
        [[nodiscard]] bool is_busy() const noexcept {
            return handle_ && !handle_.done() && handle_.promise().awaited_flags_ != nullptr;
        }

        /// Gates on promise_released alone: the result may be error_set only, and such a state
        /// must still drain. The refusal belongs at extraction (owning_awaiter::await_resume).
        [[nodiscard]] bool is_awaited_ready() const noexcept {
            if (!handle_ || handle_.done()) {
                return false;
            }
            auto* flags = handle_.promise().awaited_flags_;
            if (!flags) {
                return false;
            }
            return flags->load(std::memory_order_acquire) & detail::state_flags::promise_released;
        }

        /// The deepest awaited continuation, or null.
        [[nodiscard]] detail::coroutine_handle<> take_awaited_continuation() noexcept {
            if (!handle_ || handle_.done()) {
                return nullptr;
            }
            auto* cont_ptr = handle_.promise().awaited_continuation_;
            if (!cont_ptr) {
                return nullptr;
            }
            return cont_ptr->exchange(nullptr, std::memory_order_acq_rel);
        }
    };

} // namespace actor_zeta
