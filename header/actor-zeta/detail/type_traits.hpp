#pragma once

#include <actor-zeta/config.hpp>
#include <type_traits>
#include <utility>

namespace actor_zeta { namespace type_traits {

    using std::decay_t;
    using std::enable_if_t;
    using std::index_sequence;
    using std::make_index_sequence;
    using std::remove_reference_t;

    template<typename _Tp>
    using remove_cvref_t = typename std::remove_cv<typename std::remove_reference<_Tp>::type>::type; // C++ 20

    template<typename...>
    struct _or_;

    template<>
    struct _or_<>
        : public std::false_type {};

    template<typename _B1>
    struct _or_<_B1>
        : public _B1 {};

    template<typename _B1, typename _B2>
    struct _or_<_B1, _B2>
        : public std::conditional<_B1::value, _B1, _B2>::type {};

    template<typename _B1, typename _B2, typename _B3, typename... _Bn>
    struct _or_<_B1, _B2, _B3, _Bn...>
        : public std::conditional<_B1::value, _B1, _or_<_B2, _B3, _Bn...>>::type {};

    template<typename...>
    using void_t = void;

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

}} // namespace actor_zeta::type_traits
