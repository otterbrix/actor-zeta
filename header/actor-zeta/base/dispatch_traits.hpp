#pragma once

#include <cstdint>
#include <actor-zeta/detail/type_list.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/mailbox/id.hpp>
#include <actor-zeta/base/forwards.hpp>

namespace actor_zeta {

    /// @brief Compile-time map entry: (MethodPtr -> ActionId)
    template<auto MethodPtr>
    struct method_map_entry {};

    template<auto MethodPtr>
    using method = method_map_entry<MethodPtr>;

    template<auto... MethodPtrs>
    struct dispatch_traits {
        using methods = type_traits::type_list<method_map_entry<MethodPtrs>...>;
    };

    namespace detail {
        // Helper: unwrap unique_future<T> to T (for PHASE 3 handler integration)
        template<typename T>
        using unwrap_future_t = typename std::conditional_t<
            type_traits::is_unique_future_v<T>,
            typename type_traits::is_unique_future<T>::value_type,
            T
        >;

        template<auto SearchPtr, auto CurrentPtr>
        struct is_same_method_ptr : std::false_type {};

        template<auto Ptr>
        struct is_same_method_ptr<Ptr, Ptr> : std::true_type {};

        template<auto SearchPtr, auto... MethodPtrs>
        static constexpr uint64_t find_method_index() {
            constexpr bool matches[] = {is_same_method_ptr<SearchPtr, MethodPtrs>::value...};

            for (std::size_t i = 0; i < sizeof...(MethodPtrs); ++i) {
                if (matches[i]) {
                    return static_cast<uint64_t>(i);
                }
            }
            return 0;
        }
    }

    /// @code
    /// class MyActor {
    ///     struct dispatch_traits {
    ///         using methods = type_list<
    ///             method<&MyActor::insert>,    // ActionId = 0
    ///             method<&MyActor::remove>     // ActionId = 1
    ///         >;
    ///     };
    /// };
    ///
    /// case msg_id<MyActor, &MyActor::insert>:
    /// @endcode
    template<typename Actor, auto MethodPtr, typename MethodList>
    struct action_id_impl;

    template<typename Actor, auto SearchPtr, auto... MethodPtrs>
    struct action_id_impl<Actor, SearchPtr, type_traits::type_list<method_map_entry<MethodPtrs>...>> {
        static constexpr uint64_t value = detail::find_method_index<SearchPtr, MethodPtrs...>();
    };

    template<typename Actor, auto MethodPtr>
    inline constexpr auto msg_id = mailbox::make_message_id(
        action_id_impl<Actor, MethodPtr, typename Actor::dispatch_traits::methods>::value
    );

    template<typename Actor, typename Method, typename MethodList>
    struct runtime_dispatch_helper;

    // Forward declaration для detail::dispatch_method_impl
    namespace detail {
        template<typename Actor, auto MethodPtr, uint64_t ActionId, typename ActorPtr, typename Sender, typename... Args>
        auto dispatch_method_impl(ActorPtr* actor, Sender sender, Args&&... args)
            -> typename Actor::template unique_future<
                unwrap_future_t<typename type_traits::callable_trait<decltype(MethodPtr)>::result_type>>;
    }

    // Базовый случай - метод не найден
    template<typename Actor, typename Method>
    struct runtime_dispatch_helper<Actor, Method, type_traits::type_list<>> {
        template<typename ActorPtr, typename Sender, typename... Args>
        static auto dispatch(Method method, ActorPtr* actor, Sender sender, Args&&... args)
            -> typename Actor::template unique_future<void> {
            (void)method; (void)actor; (void)sender; (void)sizeof...(args);
            // Method not found - should not happen in correct code
            assert(false && "Method not found in dispatch_traits");
            // Return dummy future (never reached)
            return typename Actor::template unique_future<void>{nullptr, nullptr};
        }
    };

    template<typename Actor, typename Method, typename ActorPtr, typename Sender, typename... Args>
    struct dispatch_one_impl {
        // Base case - no methods left
        template<std::size_t... Is>
        static auto dispatch(Method method, ActorPtr* actor, Sender sender, std::index_sequence<>, Args&&... args)
            -> typename Actor::template unique_future<
                detail::unwrap_future_t<typename type_traits::callable_trait<Method>::result_type>>
        {
            using result_type = typename type_traits::callable_trait<Method>::result_type;
            (void)method; (void)actor; (void)sender; (void)sizeof...(args);
            // Method not found - this should never happen in correct code
            assert(false && "Method not found in dispatch_traits");
            std::abort();  // Unreachable, but satisfies return type
        }

        // Recursive case - try first method, then rest
        template<auto FirstMethod, auto... RestMethods, std::size_t FirstIndex, std::size_t... RestIndices>
        static auto dispatch(Method method, ActorPtr* actor, Sender sender,
                            std::index_sequence<FirstIndex, RestIndices...>, Args&&... args)
            -> typename Actor::template unique_future<
                detail::unwrap_future_t<typename type_traits::callable_trait<Method>::result_type>>
        {
            // Check if this method matches
            if constexpr (std::is_same<Method, decltype(FirstMethod)>::value) {
                if (method == FirstMethod) {
                    // Match! Dispatch and return
                    return detail::dispatch_method_impl<Actor, FirstMethod, FirstIndex>(
                        actor, sender, std::forward<Args>(args)...);
                }
            }

            // No match, try remaining methods
            return dispatch<RestMethods...>(
                method, actor, sender,
                std::index_sequence<RestIndices...>{},
                std::forward<Args>(args)...);
        }
    };

    template<typename Actor, typename Method, auto... MethodPtrs, typename ActorPtr, typename Sender, typename... Args, std::size_t... Is>
    static auto dispatch_impl(Method method, ActorPtr* actor, Sender sender, std::index_sequence<Is...>, Args&&... args)
        -> typename Actor::template unique_future<
            detail::unwrap_future_t<typename type_traits::callable_trait<Method>::result_type>>
    {
        return dispatch_one_impl<Actor, Method, ActorPtr, Sender, Args...>::template dispatch<MethodPtrs...>(
            method, actor, sender,
            std::index_sequence<Is...>{},
            std::forward<Args>(args)...);
    }

    template<typename Actor, typename Method, auto... MethodPtrs>
    struct runtime_dispatch_helper<Actor, Method, type_traits::type_list<method_map_entry<MethodPtrs>...>> {
        template<typename ActorPtr, typename Sender, typename... Args>
        static auto dispatch(Method method, ActorPtr* actor, Sender sender, Args&&... args)
            -> typename Actor::template unique_future<
                detail::unwrap_future_t<typename type_traits::callable_trait<Method>::result_type>>
        {
            return dispatch_impl<Actor, Method, MethodPtrs...>(
                method, actor, sender,
                std::index_sequence_for<method_map_entry<MethodPtrs>...>{},
                std::forward<Args>(args)...);
        }

        // Перегрузка для address_t
        template<typename Sender, typename... Args>
        static auto dispatch(Method method, base::address_t target, Sender sender, Args&&... args)
            -> typename Actor::template unique_future<
                detail::unwrap_future_t<typename type_traits::callable_trait<Method>::result_type>>
        {
            auto* actor = static_cast<Actor*>(target.operator->());
            return dispatch_impl<Actor, Method, MethodPtrs...>(
                method, actor, sender,
                std::index_sequence_for<method_map_entry<MethodPtrs>...>{},
                std::forward<Args>(args)...);
        }
    };

} // namespace actor_zeta