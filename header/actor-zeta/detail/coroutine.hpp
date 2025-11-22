#pragma once

#include <actor-zeta/config.hpp>

// NOTE: Coroutines are now REQUIRED (static_assert in config.hpp)
// HAVE_EXPERIMENTAL_COROUTINES distinguishes between std and experimental

#if HAVE_EXPERIMENTAL_COROUTINES
    // Experimental coroutines (GCC 9, Clang 8-13)
#  include <experimental/coroutine>

namespace actor_zeta {
namespace detail {

    template<typename Promise = void>
    using coroutine_handle = std::experimental::coroutine_handle<Promise>;

    template<typename... Ts>
    using coroutine_traits = std::experimental::coroutine_traits<Ts...>;

    using suspend_always = std::experimental::suspend_always;
    using suspend_never = std::experimental::suspend_never;

} // namespace detail
} // namespace actor_zeta

#else
    // Standard C++20 coroutines (GCC 10+, Clang 14+, MSVC 2019 16.8+)
#  include <coroutine>

namespace actor_zeta {
namespace detail {

    template<typename Promise = void>
    using coroutine_handle = std::coroutine_handle<Promise>;

    template<typename... Ts>
    using coroutine_traits = std::coroutine_traits<Ts...>;

    using suspend_always = std::suspend_always;
    using suspend_never = std::suspend_never;

} // namespace detail
} // namespace actor_zeta

#endif // HAVE_EXPERIMENTAL_COROUTINES