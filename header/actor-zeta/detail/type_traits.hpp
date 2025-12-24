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

// Forward declarations for type traits
namespace actor_zeta {
    template<typename T>
    class unique_future;

    template<typename T>
    class generator;
}

namespace actor_zeta { namespace type_traits {

    template<typename T>
    struct is_unique_future : std::false_type {};

    template<typename T>
    struct is_unique_future<unique_future<T>> : std::true_type {
        using value_type = T;
    };

    template<typename T>
    constexpr bool is_unique_future_v = is_unique_future<T>::value;

    template<typename T>
    concept unique_future_type = is_unique_future_v<T>;

    template<typename T>
    struct is_generator : std::false_type {};

    template<typename T>
    struct is_generator<generator<T>> : std::true_type {
        using value_type = T;
    };

    template<typename T>
    constexpr bool is_generator_v = is_generator<T>::value;

    template<typename T>
    concept generator_type = is_generator_v<T>;

    template<typename T>
    struct unwrap_generator {
        using type = T;
    };

    template<typename T>
    struct unwrap_generator<generator<T>> {
        using type = T;
    };

    template<typename T>
    using unwrap_generator_t = typename unwrap_generator<T>::type;

}} // namespace actor_zeta::type_traits
