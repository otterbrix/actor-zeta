#pragma once

#include <actor-zeta/config.hpp>
#include <concepts>
#include <type_traits>
#include <utility>

namespace actor_zeta { namespace type_traits {

    struct internal_construct_tag {};

    using std::decay_t;
    using std::index_sequence;
    using std::make_index_sequence;
    using std::remove_reference_t;
    using std::remove_cvref_t; // C++20

    struct erased_type {};

    struct allocator_arg_t {
        explicit allocator_arg_t() = default;
    };

    constexpr allocator_arg_t allocator_arg = allocator_arg_t();

}} // namespace actor_zeta::type_traits

// Forward declaration for is_unique_future
namespace actor_zeta {
    template<typename T>
    class unique_future;
}

namespace actor_zeta { namespace type_traits {

    /// @brief Type trait to detect unique_future<T>
    /// @note Used for handler integration to detect methods returning futures
    template<typename T>
    struct is_unique_future : std::false_type {};

    template<typename T>
    struct is_unique_future<unique_future<T>> : std::true_type {
        using value_type = T;
    };

    /// @brief Helper variable template (C++17)
    template<typename T>
    constexpr bool is_unique_future_v = is_unique_future<T>::value;

    /// @brief Concept to detect unique_future<T>
    template<typename T>
    concept unique_future_type = is_unique_future_v<T>;

}} // namespace actor_zeta::type_traits
