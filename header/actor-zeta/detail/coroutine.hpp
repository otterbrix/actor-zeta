#pragma once

#include <actor-zeta/config.hpp>

#if HAVE_EXPERIMENTAL_COROUTINES
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