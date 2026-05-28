#pragma once

// run_until_complete — drive a unique_future to ready by repeatedly invoking the
// caller's `pump` (typically `[&]{ actor->resume(n); }`), then take_ready() the value.

#include <cassert>
#include <cstddef>
#include <type_traits>
#include <utility>

#include <actor-zeta/detail/future.hpp>

namespace actor_zeta {

    namespace detail {
        // Debug-only iteration cap to surface a mis-wired pump (release: unbounded).
        inline constexpr std::size_t run_until_complete_max_iterations = 100'000'000;
    } // namespace detail

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
