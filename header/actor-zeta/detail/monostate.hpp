#pragma once

// Seastar-style monostate for unified void/non-void future handling
// This eliminates the need for full template specialization for void

#include <type_traits>

namespace actor_zeta { namespace detail {

    /// @brief Empty type to represent void in generic contexts
    /// Used instead of void to allow unified future_state<T> implementation.
    /// Seastar calls this "monostate" - a type with exactly one value.
    struct monostate {
        constexpr monostate() noexcept = default;
        constexpr bool operator==(monostate) const noexcept { return true; }
        constexpr bool operator!=(monostate) const noexcept { return false; }
        constexpr bool operator<(monostate) const noexcept { return false; }
        constexpr bool operator>(monostate) const noexcept { return false; }
        constexpr bool operator<=(monostate) const noexcept { return true; }
        constexpr bool operator>=(monostate) const noexcept { return true; }
    };

    /// @brief Type trait to convert void to monostate, leave other types as-is
    /// @tparam T The type to convert
    ///
    /// Usage:
    ///   future_stored_type_t<void>  -> monostate
    ///   future_stored_type_t<int>   -> int
    ///   future_stored_type_t<Foo>   -> Foo
    template<typename T>
    struct future_stored_type {
        using type = T;
    };

    template<>
    struct future_stored_type<void> {
        using type = monostate;
    };

    template<typename T>
    using future_stored_type_t = typename future_stored_type<T>::type;

    /// @brief Check if type is monostate
    template<typename T>
    struct is_monostate : std::false_type {};

    template<>
    struct is_monostate<monostate> : std::true_type {};

    template<typename T>
    inline constexpr bool is_monostate_v = is_monostate<T>::value;

    /// @brief Tuple type for future results
    /// For monostate: empty tuple std::tuple<>
    /// For T: single-element tuple std::tuple<T>
    template<typename T>
    struct future_tuple_type {
        using type = std::tuple<T>;
    };

    template<>
    struct future_tuple_type<monostate> {
        using type = std::tuple<>;
    };

    template<typename T>
    using future_tuple_type_t = typename future_tuple_type<T>::type;

    /// @brief Get return type for get0() - void for monostate, T otherwise
    template<typename T>
    struct get0_return_type {
        using type = T;
        static T get0(T&& v) { return std::move(v); }
    };

    template<>
    struct get0_return_type<monostate> {
        using type = void;
        static void get0(monostate) { /* no-op */ }
    };

    /// @brief Wrap reference types in reference_wrapper (Seastar pattern)
    /// Regular types: T -> T
    /// Reference types: T& -> std::reference_wrapper<T>
    template<typename T>
    struct maybe_wrap_ref {
        using type = T;
    };

    template<typename T>
    struct maybe_wrap_ref<T&> {
        using type = std::reference_wrapper<T>;
    };

    template<typename T>
    using maybe_wrap_ref_t = typename maybe_wrap_ref<T>::type;

    /// @brief Trait for trivially movable and destructible types
    /// Used for memmove optimization in state moves
    template<typename T>
    struct is_trivially_move_constructible_and_destructible {
        static constexpr bool value =
            std::is_trivially_move_constructible_v<T> &&
            std::is_trivially_destructible_v<T>;
    };

    template<typename T>
    inline constexpr bool is_trivially_move_constructible_and_destructible_v =
        is_trivially_move_constructible_and_destructible<T>::value;

    // monostate is trivially everything
    static_assert(std::is_trivially_default_constructible_v<monostate>);
    static_assert(std::is_trivially_copy_constructible_v<monostate>);
    static_assert(std::is_trivially_move_constructible_v<monostate>);
    static_assert(std::is_trivially_destructible_v<monostate>);
    static_assert(is_trivially_move_constructible_and_destructible_v<monostate>);

}} // namespace actor_zeta::detail