#pragma once

// run_until_complete — a zero-blocking coroutine pump.
//
// Replaces post-pump blocking get()/wait() with an explicit driver: the caller supplies a
// `pump` callable (typically `[&]{ actor->resume(n); }`) that is invoked repeatedly until the
// future is ready, then the value is taken non-blocking via take_ready().
//
// This is the root driver for the co_await model: tests and examples poll readiness with
// is_ready() and pump (an actor's resume(), an io_context, or a plain yield) until ready.

#include <cassert>
#include <cstddef>
#include <type_traits>
#include <utility>

#include <actor-zeta/detail/future.hpp>

namespace actor_zeta {

    namespace detail {
        // Debug-only bound to surface a mis-wired pump (pumping the wrong actor would never
        // make the future ready and would otherwise hang forever). Release builds: no bound.
        inline constexpr std::size_t run_until_complete_max_iterations = 100'000'000;
    } // namespace detail

    // Pump `f` to completion, then take the value (non-void).
    template<typename T, typename Pump>
        requires(!std::is_void_v<T>)
    [[nodiscard]] T run_until_complete(unique_future<T>& f, Pump&& pump) {
#ifndef NDEBUG
        std::size_t iterations = 0;
#endif
        while (!f.is_ready()) {
            pump();
#ifndef NDEBUG
            assert(++iterations < detail::run_until_complete_max_iterations
                   && "run_until_complete pumped too many times — wrong actor/scheduler?");
#endif
        }
        return std::move(f).take_ready();
    }

    // Pump `f` to completion (void).
    template<typename T, typename Pump>
        requires(std::is_void_v<T>)
    void run_until_complete(unique_future<T>& f, Pump&& pump) {
#ifndef NDEBUG
        std::size_t iterations = 0;
#endif
        while (!f.is_ready()) {
            pump();
#ifndef NDEBUG
            assert(++iterations < detail::run_until_complete_max_iterations
                   && "run_until_complete pumped too many times — wrong actor/scheduler?");
#endif
        }
        std::move(f).take_ready();
    }

} // namespace actor_zeta
