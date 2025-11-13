#pragma once

#include <actor-zeta/config.hpp>

// C++20 coroutines
#if HAVE_STD_COROUTINES
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

#endif // HAVE_STD_COROUTINES

// Fallback to experimental coroutines
#if !HAVE_STD_COROUTINES && defined(__cpp_coroutines)
#  if defined(__has_include)
#    if __has_include(<experimental/coroutine>)
#      include <experimental/coroutine>

namespace actor_zeta {
namespace detail {

    // Direct aliases from std::experimental, no intermediate std:: import
    template<typename Promise = void>
    using coroutine_handle = std::experimental::coroutine_handle<Promise>;

    template<typename... Ts>
    using coroutine_traits = std::experimental::coroutine_traits<Ts...>;

    using suspend_always = std::experimental::suspend_always;
    using suspend_never = std::experimental::suspend_never;

} // namespace detail
} // namespace actor_zeta

#      undef HAVE_STD_COROUTINES
#      define HAVE_STD_COROUTINES 1
#    endif
#  endif
#endif