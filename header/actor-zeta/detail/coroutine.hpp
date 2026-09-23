#pragma once

#include <actor-zeta/config.hpp>

#if HAVE_EXPERIMENTAL_COROUTINES

// <experimental/coroutine> has no noop_coroutine(); the stub below returns a
// null handle, and returning null from await_suspend in a symmetric-transfer
// position is undefined behaviour -- the caller resumes it unconditionally.
// Every symmetric-transfer site in future.hpp and future_awaiters.hpp depends on
// it, so refuse the configuration rather than miscompile silently.
#error "actor-zeta does not support <experimental/coroutine>: detail::noop_coroutine() cannot be implemented on it. Build with a toolchain providing <coroutine> (GCC 10+, Clang 14+, MSVC 2019 16.8+)."

#include <experimental/coroutine>

namespace actor_zeta {
namespace detail {

    template<typename Promise = void>
    using coroutine_handle = std::experimental::coroutine_handle<Promise>;

    template<typename... Ts>
    using coroutine_traits = std::experimental::coroutine_traits<Ts...>;

    using suspend_always = std::experimental::suspend_always;
    using suspend_never = std::experimental::suspend_never;

    inline coroutine_handle<> noop_coroutine() noexcept {
        return {}; // experimental lacks noop_coroutine
    }

}
} // namespace actor_zeta::detail

#else
#include <coroutine>

namespace actor_zeta {
namespace detail {

    template<typename Promise = void>
    using coroutine_handle = std::coroutine_handle<Promise>;

    template<typename... Ts>
    using coroutine_traits = std::coroutine_traits<Ts...>;

    using suspend_always = std::suspend_always;
    using suspend_never = std::suspend_never;

    inline coroutine_handle<> noop_coroutine() noexcept {
        return std::noop_coroutine();
    }

}
} // namespace actor_zeta::detail

#endif // HAVE_EXPERIMENTAL_COROUTINES