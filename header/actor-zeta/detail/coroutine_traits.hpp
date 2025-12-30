#pragma once

#include <concepts>
#include <type_traits>

#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/promise_concepts.hpp>

namespace actor_zeta::actor {
    // Forward declaration
    template<typename Derived>
    class actor_mixin;

    template<typename Derived, typename Traits, typename Type>
    class cooperative_actor;
}

namespace actor_zeta::detail {

    // Helper to check if type derives from actor_mixin
    template<typename T>
    concept is_actor_type = requires {
        typename T::dispatch_traits;
    };

} // namespace actor_zeta::detail

// ============================================================================
// std::coroutine_traits specialization for actors WITH custom promise_type
// ============================================================================

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

// ============================================================================
// std::coroutine_traits specialization for actors WITHOUT custom promise_type
// Uses the default promise_type from unique_future<T>
// ============================================================================

// Note: This is actually not needed because the default behavior
// (using unique_future<T>::promise_type) already handles this case.
// The specialization above only overrides when a custom promise_type exists.