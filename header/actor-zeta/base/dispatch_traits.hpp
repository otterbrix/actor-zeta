#pragma once

#include <cstdint>
#include <actor-zeta/detail/type_list.hpp>
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

    template<typename Actor, typename Method>
    struct runtime_dispatch_helper<Actor, Method, type_traits::type_list<>> {
        template<typename ActorPtr, typename Sender, typename... Args>
        static bool dispatch(Method method, ActorPtr* actor, Sender sender, Args&&... args) {
            (void)method; (void)actor; (void)sender; (void)sizeof...(args);
            return false;
        }
    };

    namespace detail {
        template<typename Actor, auto MethodPtr, uint64_t ActionId, typename ActorPtr, typename Sender, typename... Args>
        bool dispatch_method_impl(ActorPtr* actor, Sender sender, Args&&... args);
    }

    template<typename Actor, typename Method, auto MethodPtr, uint64_t Index, typename ActorPtr, typename Sender, typename... Args>
    static bool try_dispatch_one(Method method, ActorPtr* actor, Sender sender, Args&&... args) {
        if constexpr (std::is_same<Method, decltype(MethodPtr)>::value) {
            if (method == MethodPtr) {
                return detail::dispatch_method_impl<Actor, MethodPtr, Index>(
                    actor, sender, std::forward<Args>(args)...);
            }
        }
        return false;
    }

    template<typename Actor, typename Method, auto... MethodPtrs, typename ActorPtr, typename Sender, typename... Args, std::size_t... Is>
    static bool dispatch_impl(Method method, ActorPtr* actor, Sender sender, std::index_sequence<Is...>, Args&&... args) {
        return (try_dispatch_one<Actor, Method, MethodPtrs, Is>(method, actor, sender, std::forward<Args>(args)...) || ...);
    }

    template<typename Actor, typename Method, auto... MethodPtrs>
    struct runtime_dispatch_helper<Actor, Method, type_traits::type_list<method_map_entry<MethodPtrs>...>> {
        template<typename ActorPtr, typename Sender, typename... Args>
        static bool dispatch(Method method, ActorPtr* actor, Sender sender, Args&&... args) {
            return dispatch_impl<Actor, Method, MethodPtrs...>(
                method, actor, sender,
                std::index_sequence_for<method_map_entry<MethodPtrs>...>{},
                std::forward<Args>(args)...);
        }

        // Перегрузка для address_t
        template<typename Sender, typename... Args>
        static bool dispatch(Method method, base::address_t target, Sender sender, Args&&... args) {
            auto* actor = static_cast<Actor*>(target.operator->());
            return dispatch_impl<Actor, Method, MethodPtrs...>(
                method, actor, sender,
                std::index_sequence_for<method_map_entry<MethodPtrs>...>{},
                std::forward<Args>(args)...);
        }
    };

} // namespace actor_zeta