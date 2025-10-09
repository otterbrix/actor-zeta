#pragma once

namespace actor_zeta { namespace detail {

    template<typename... args>
    void ignore_unused(args const&...) {}

    template<typename... args>
    void ignore_unused() {}

}} // namespace actor_zeta::detail
