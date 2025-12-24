#pragma once

#include <concepts>
#include <cstdint>

#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/generator.hpp>
#include <actor-zeta/detail/ignore_unused.hpp>
#include <actor-zeta/detail/type_list.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/mailbox/id.hpp>

namespace actor_zeta {

    template<auto MethodPtr>
    struct method_map_entry {};

    template<auto MethodPtr>
    using method = method_map_entry<MethodPtr>;

    namespace detail {

        template<typename T>
        concept const_lvalue_ref = std::is_lvalue_reference_v<T> && std::is_const_v<std::remove_reference_t<T>>;

        template<typename... Args>
        concept has_any_const_lvalue_ref = (const_lvalue_ref<Args> || ...);

        namespace type_list_check {
            template<typename ArgsList>
            struct has_const_ref_impl;

            template<typename... Args>
            struct has_const_ref_impl<type_traits::type_list<Args...>> {
                static constexpr bool value = has_any_const_lvalue_ref<Args...>;
            };
        }

        template<typename ArgsList>
        concept type_list_has_const_lvalue_ref = type_list_check::has_const_ref_impl<ArgsList>::value;

        template<auto MethodPtr>
        struct method_return_type_check {
            using trait = type_traits::callable_trait<decltype(MethodPtr)>;
            using result_type = typename trait::result_type;

            static constexpr bool returns_unique_future = type_traits::is_unique_future_v<result_type>;
            static constexpr bool returns_generator = type_traits::is_generator_v<result_type>;
            static constexpr bool is_valid = returns_unique_future || returns_generator;
        };

        template<auto MethodPtr>
        struct coroutine_parameter_check {
            using trait = type_traits::callable_trait<decltype(MethodPtr)>;
            using result_type = typename trait::result_type;
            using args_types = typename trait::args_types;

            static constexpr bool is_coroutine = type_traits::is_unique_future_v<result_type>;
            static constexpr bool has_const_ref = type_list_has_const_lvalue_ref<args_types>;
            static constexpr bool is_safe = !is_coroutine || !has_const_ref;
        };
    } // namespace detail

    template<auto... MethodPtrs>
    struct dispatch_traits {
        using methods = type_traits::type_list<method_map_entry<MethodPtrs>...>;

        static_assert(
            (detail::method_return_type_check<MethodPtrs>::is_valid && ...),
            "All actor methods must return unique_future<T> or generator<T>. "
            "Raw void or value returns are not allowed. "
            "All actor methods must be coroutines using co_return or co_yield.");

        static_assert(
            (detail::coroutine_parameter_check<MethodPtrs>::is_safe && ...),
            "Coroutine methods must not have const& parameters. "
            "After co_await, message is destroyed and const& becomes dangling. Use by-value instead.");
    };

    namespace detail {
        template<typename T>
        using unwrap_future_t = typename std::conditional_t<
            type_traits::is_unique_future_v<T>,
            typename type_traits::is_unique_future<T>::value_type,
            T>;

        template<typename Actor, typename ResultType>
        struct dispatch_result_type {
            using type = typename Actor::template unique_future<unwrap_future_t<ResultType>>;
        };

        template<typename Actor, typename T>
        struct dispatch_result_type<Actor, generator<T>> {
            using type = generator<T>;
        };

        template<typename Actor, typename ResultType>
        using dispatch_result_t = typename dispatch_result_type<Actor, ResultType>::type;

        template<auto SearchPtr, auto CurrentPtr>
        struct is_same_ptr {
            static constexpr bool value = false;
        };

        template<auto Ptr>
        struct is_same_ptr<Ptr, Ptr> {
            static constexpr bool value = true;
        };

        template<auto SearchPtr, auto CurrentPtr>
        inline constexpr bool is_same_ptr_v = is_same_ptr<SearchPtr, CurrentPtr>::value;

        template<auto SearchPtr, auto... MethodPtrs>
        static constexpr uint64_t find_method_index() {
            constexpr bool matches[] = {is_same_ptr_v<SearchPtr, MethodPtrs>...};

            for (std::size_t i = 0; i < sizeof...(MethodPtrs); ++i) {
                if (matches[i]) {
                    return static_cast<uint64_t>(i);
                }
            }
            return 0;
        }
    } // namespace detail

    template<typename Actor, auto MethodPtr, typename MethodList>
    struct action_id_impl;

    template<typename Actor, auto SearchPtr, auto... MethodPtrs>
    struct action_id_impl<Actor, SearchPtr, type_traits::type_list<method_map_entry<MethodPtrs>...>> {
        static constexpr uint64_t value = detail::find_method_index<SearchPtr, MethodPtrs...>();
    };

    template<typename Actor, auto MethodPtr>
    inline constexpr auto msg_id = mailbox::make_message_id(
        action_id_impl<Actor, MethodPtr, typename Actor::dispatch_traits::methods>::value);

    template<typename Actor, typename Method, typename MethodList>
    struct runtime_dispatch_helper;

    namespace detail {
        template<typename Actor, auto MethodPtr, uint64_t ActionId, typename ActorPtr, typename Sender, typename... Args>
        auto dispatch_method_impl(ActorPtr* actor, Sender sender, Args&&... args)
            -> dispatch_result_t<Actor, typename type_traits::callable_trait<decltype(MethodPtr)>::result_type>;
    }

    template<typename Actor, typename Method>
    struct runtime_dispatch_helper<Actor, Method, type_traits::type_list<>> {
        template<typename ActorPtr, typename Sender, typename... Args>
        static auto dispatch(Method method, ActorPtr* actor, Sender sender, Args&&... args)
            -> detail::dispatch_result_t<Actor, typename type_traits::callable_trait<Method>::result_type> {
            detail::ignore_unused(method, actor, sender, sizeof...(args));
            assert(false && "Method not found in dispatch_traits");
            std::abort();
        }
    };

    template<typename Actor, typename Method, typename ActorPtr, typename Sender, typename... Args>
    struct dispatch_one_impl {
        using result_type = typename type_traits::callable_trait<Method>::result_type;
        using dispatch_return_type = detail::dispatch_result_t<Actor, result_type>;

        template<std::size_t... Is>
        static auto dispatch(Method method, ActorPtr* actor, Sender sender, std::index_sequence<>, Args&&... args)
            -> dispatch_return_type {
            detail::ignore_unused(method, actor, sender, sizeof...(args));
            assert(false && "Method not found in dispatch_traits");
            std::abort();
        }

        template<auto FirstMethod, auto... RestMethods, std::size_t FirstIndex, std::size_t... RestIndices>
        static auto dispatch(Method method, ActorPtr* actor, Sender sender,
                             std::index_sequence<FirstIndex, RestIndices...>, Args&&... args)
            -> dispatch_return_type {
            if constexpr (std::same_as<Method, decltype(FirstMethod)>) {
                if (method == FirstMethod) {
                    return detail::dispatch_method_impl<Actor, FirstMethod, FirstIndex>(
                        actor, sender, std::forward<Args>(args)...);
                }
            }

            return dispatch<RestMethods...>(
                method, actor, sender,
                std::index_sequence<RestIndices...>{},
                std::forward<Args>(args)...);
        }
    };

    template<typename Actor, typename Method, auto... MethodPtrs, typename ActorPtr, typename Sender, typename... Args, std::size_t... Is>
    static auto dispatch_impl(Method method, ActorPtr* actor, Sender sender, std::index_sequence<Is...>, Args&&... args)
        -> detail::dispatch_result_t<Actor, typename type_traits::callable_trait<Method>::result_type> {
        return dispatch_one_impl<Actor, Method, ActorPtr, Sender, Args...>::template dispatch<MethodPtrs...>(
            method, actor, sender,
            std::index_sequence<Is...>{},
            std::forward<Args>(args)...);
    }

    template<typename Actor, typename Method, auto... MethodPtrs>
    struct runtime_dispatch_helper<Actor, Method, type_traits::type_list<method_map_entry<MethodPtrs>...>> {
        using result_type = typename type_traits::callable_trait<Method>::result_type;
        using dispatch_return_type = detail::dispatch_result_t<Actor, result_type>;

        template<typename ActorPtr, typename Sender, typename... Args>
        static auto dispatch(Method method, ActorPtr* actor, Sender sender, Args&&... args)
            -> dispatch_return_type {
            return dispatch_impl<Actor, Method, MethodPtrs...>(
                method, actor, sender,
                std::index_sequence_for<method_map_entry<MethodPtrs>...>{},
                std::forward<Args>(args)...);
        }

        template<typename Sender, typename... Args>
        static auto dispatch(Method method, actor::address_t target, Sender sender, Args&&... args)
            -> dispatch_return_type {
            auto* actor = static_cast<Actor*>(target.get());
            return dispatch_impl<Actor, Method, MethodPtrs...>(
                method, actor, sender,
                std::index_sequence_for<method_map_entry<MethodPtrs>...>{},
                std::forward<Args>(args)...);
        }
    };

} // namespace actor_zeta