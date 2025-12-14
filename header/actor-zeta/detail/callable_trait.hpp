#pragma once

#include <actor-zeta/detail/type_list.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <concepts>
#include <functional>

namespace actor_zeta { namespace type_traits {

    template<class Functor>
    struct callable_trait;

    template<class R, class... Args>
    struct callable_trait<R(Args...)> {
        using result_type = R;
        using args_types = type_list<Args...>;
        using fun_sig = R(Args...);
        using fun_type = std::function<R(Args...)>;
        static constexpr size_t number_of_arguments = type_list_size_v<args_types>;
    };

    template<class C, typename R, class... Args>
    struct callable_trait<R (C::*)(Args...) const noexcept>
        : callable_trait<R(Args...)> {
        using class_type = C;
    };

    template<class C, typename R, class... Args>
    struct callable_trait<R (C::*)(Args...) noexcept> : callable_trait<R(Args...)> {
        using class_type = C;
    };

    template<class C, typename R, class... Args>
    struct callable_trait<R (C::*)(Args...) const> : callable_trait<R(Args...)> {
        using class_type = C;
    };

    template<class C, typename R, class... Args>
    struct callable_trait<R (C::*)(Args...)> : callable_trait<R(Args...)> {
        using class_type = C;
    };

    template<class R, class... Args>
    struct callable_trait<R (*)(Args...) noexcept> : callable_trait<R(Args...)> {};

    template<class R, class... Args>
    struct callable_trait<R (*)(Args...)> : callable_trait<R(Args...)> {};

    /// @brief Concept to check if type has operator()
    template<typename T>
    concept has_call_operator = requires(T t) {
        { &T::operator() };
    };

    /// @brief Type trait for has_apply_operator (using concept)
    template<class T>
    struct has_apply_operator final {
        static constexpr bool value = has_call_operator<T>;
    };

    template<class T,
             bool IsFun = std::is_function_v<T> || std::is_function_v<std::remove_pointer_t<T>> || std::is_member_function_pointer_v<T>,
             bool HasApplyOp = has_call_operator<T>>
    struct get_callable_trait_helper {
        using type = callable_trait<T>;
        using result_type = typename type::result_type;
        using args_types = typename type::args_types;
        using fun_type = typename type::fun_type;
        using fun_sig = typename type::fun_sig;
        static constexpr size_t number_of_arguments = type_list_size_v<args_types>;
    };

    template<class T>
    struct get_callable_trait_helper<T, false, true> {
        using type = callable_trait<decltype(&T::operator())>;
        using class_type = typename type::class_type;
        using result_type = typename type::result_type;
        using args_types = typename type::args_types;
        using fun_type = typename type::fun_type;
        using fun_sig = typename type::fun_sig;
        static constexpr size_t number_of_arguments = type_list_size_v<args_types>;
    };

    template<class T>
    struct get_callable_trait_helper<T, false, false> {};

    template<class T>
    struct get_callable_trait : get_callable_trait_helper<decay_t<T>> {};

    template<class T>
    using get_callable_trait_t = typename get_callable_trait<T>::type;

    /// @brief Concept to check if type is callable (has callable trait)
    template<typename T>
    concept callable_type = requires {
        typename get_callable_trait<T>::type;
    };

    /// @brief Type trait for is_callable (using concept)
    template<class T>
    struct is_callable final {
        static constexpr bool value = callable_type<decay_t<T>>;
    };

    /// @brief Concept to check if F is callable with given args
    template<typename F, typename... Args>
    concept callable_with = requires(F& f, Args... args) {
        { f(args...) };
    };

    /// @brief Type trait for is_callable_with (using concept)
    template<class F, class... args>
    struct is_callable_with final {
        static constexpr bool value = callable_with<F, args...>;
    };

}} // namespace actor_zeta::type_traits