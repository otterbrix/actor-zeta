#pragma once

#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <type_traits>

namespace actor_zeta {

    // Forward declaration
    template<typename T>
    class unique_future;

    namespace detail {

        // ================================================================
        // Concept for valid future value type
        // ================================================================

        template<typename T>
        concept valid_future_value_type =
            std::is_void_v<T> ||
            (
                std::move_constructible<T> &&
                std::destructible<T> &&
                !std::is_reference_v<T> &&
                !std::is_const_v<T>
            );

        // ================================================================
        // Basic promise_type requirements
        // ================================================================

        template<typename P, typename T>
        concept promise_has_get_return_object = requires(P p) {
            { p.get_return_object() } -> std::same_as<unique_future<T>>;
        };

        template<typename P>
        concept promise_has_initial_suspend = requires(P p) {
            { p.initial_suspend() } noexcept;
        };

        template<typename P>
        concept promise_has_final_suspend = requires(P p) {
            { p.final_suspend() } noexcept;
        };

        template<typename P>
        concept promise_has_unhandled_exception = requires(P p) {
            { p.unhandled_exception() } noexcept;
        };

        template<typename P>
        concept promise_has_allocation = requires(std::size_t s) {
            { P::operator new(s) } -> std::same_as<void*>;
        };

        template<typename P>
        concept promise_has_return_void = requires(P p) {
            { p.return_void() } -> std::same_as<void>;
        };

        template<typename P, typename T>
        concept promise_has_return_value = requires(P p, T val) {
            { p.return_value(std::move(val)) } -> std::same_as<void>;
        };

        // ================================================================
        // Combined concept for non-void promise_type
        // ================================================================

        template<typename P, typename T>
        concept valid_promise_type =
            valid_future_value_type<T> &&
            promise_has_get_return_object<P, T> &&
            promise_has_initial_suspend<P> &&
            promise_has_final_suspend<P> &&
            promise_has_unhandled_exception<P> &&
            promise_has_allocation<P> &&
            promise_has_return_value<P, T>;

        // ================================================================
        // Combined concept for void promise_type
        // ================================================================

        template<typename P>
        concept valid_promise_type_void =
            promise_has_get_return_object<P, void> &&
            promise_has_initial_suspend<P> &&
            promise_has_final_suspend<P> &&
            promise_has_unhandled_exception<P> &&
            promise_has_allocation<P> &&
            promise_has_return_void<P>;

        // ================================================================
        // Check if actor has custom promise_type
        // ================================================================

        template<typename Actor>
        concept has_custom_promise_type = requires {
            typename Actor::template promise_type<int>;
        };

        // ================================================================
        // Selector for promise_type
        // ================================================================

        template<typename Actor, typename T>
        struct promise_type_selector {
            // Default: use unique_future<T>::promise_type
            using type = typename unique_future<T>::promise_type;
        };

        // Specialization for actors with custom promise_type
        template<typename Actor, typename T>
            requires has_custom_promise_type<Actor>
        struct promise_type_selector<Actor, T> {
            using type = typename Actor::template promise_type<T>;
        };

        template<typename Actor, typename T>
        using promise_type_for = typename promise_type_selector<Actor, T>::type;

    } // namespace detail

} // namespace actor_zeta