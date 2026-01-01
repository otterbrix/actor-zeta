#pragma once

#include <concepts>
#include <type_traits>

#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/future.hpp>
#include <actor-zeta/detail/promise_concepts.hpp>

namespace actor_zeta::detail {

    // Helper to check if type derives from actor_mixin (has dispatch_traits)
    template<typename T>
    concept is_actor_type = requires {
        typename T::dispatch_traits;
    };

} // namespace actor_zeta::detail

// std::coroutine_traits specialization for actors WITH custom promise_type

template<typename T, typename Actor, typename... Args>
    requires actor_zeta::detail::is_actor_type<std::remove_pointer_t<Actor>>
          && actor_zeta::detail::has_custom_promise_type<std::remove_pointer_t<Actor>>
struct std::coroutine_traits<actor_zeta::unique_future<T>, Actor, Args...> {
    using actor_type = std::remove_pointer_t<Actor>;
    using promise_type = typename actor_type::template promise_type<T>;

    // Compile-time validation
    static_assert(
        std::is_void_v<T>
            ? actor_zeta::detail::valid_promise_type_void<promise_type>
            : actor_zeta::detail::valid_promise_type<promise_type, T>,
        "Actor::promise_type<T> does not satisfy promise_type requirements. "
        "Check that get_return_object() returns unique_future<T>."
    );
};

// std::coroutine_traits specialization for actors WITHOUT custom promise_type
// but WITH resource() method.
//
// This ensures GCC passes Actor& to operator new and promise constructor
// even for methods with move-only parameters (GCC bug workaround).
//
// GCC has a known issue where it doesn't pass coroutine arguments to
// operator new and promise constructor for methods with rvalue reference
// parameters to move-only types (e.g., std::unique_ptr<T>&&).
//
// This specialization forces GCC to use actor_promise<Actor> which
// explicitly requires Actor& as its first parameter, ensuring the
// actor's memory resource is always available for coroutine frame allocation.
//
// See: docs/GCC_COROUTINE_OPERATOR_NEW_BUG.md for detailed analysis.
template<typename T, typename Actor, typename... Args>
    requires actor_zeta::detail::has_resource_method<std::remove_reference_t<Actor>>
          && (!actor_zeta::detail::has_custom_promise_type<std::remove_reference_t<Actor>>)
struct std::coroutine_traits<actor_zeta::unique_future<T>, Actor&, Args...> {
    using actor_type = std::remove_reference_t<Actor>;
    using promise_type = typename actor_zeta::unique_future<T>::template actor_promise<actor_type>;
};
