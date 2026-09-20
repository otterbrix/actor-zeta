#pragma once

#include <type_traits>

namespace actor_zeta { namespace detail {

    // An element type declares `deleter_type` to say how it must be freed; the queues static_assert
    // on it, because std::default_delete on a PMR-allocated message is silent heap corruption at shutdown.
    template<class T, class = void>
    struct declared_deleter {
        static constexpr bool present = false;
        using type = void;
    };

    template<class T>
    struct declared_deleter<T, std::void_t<typename T::deleter_type>> {
        static constexpr bool present = true;
        using type = typename T::deleter_type;
    };

}} // namespace actor_zeta::detail
