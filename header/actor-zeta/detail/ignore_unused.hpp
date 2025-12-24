#pragma once

namespace actor_zeta { namespace detail {

    template<typename... args>
    constexpr void ignore_unused(args const&...) {}

    template<typename... args>
    constexpr void ignore_unused() {}

}} // namespace actor_zeta::detail
