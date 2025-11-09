#pragma once

#include <cstdint>
#include <actor-zeta/detail/type_list.hpp>
#include <actor-zeta/mailbox/id.hpp>

namespace actor_zeta {

    /// @brief Compile-time map entry: (MethodPtr -> ActionId)
    /// ActionId is generated automatically based on position in the list
    template<auto MethodPtr>
    struct method_map_entry {};

    /// @brief Short alias for method_map_entry
    template<auto MethodPtr>
    using method = method_map_entry<MethodPtr>;

    /// @brief dispatch_traits template for convenient syntax inside actor
    /// Allows writing: using dispatch_traits = dispatch_traits<&MyActor::method1, &MyActor::method2>;
    template<auto... MethodPtrs>
    struct dispatch_traits {
        using methods = type_traits::type_list<method_map_entry<MethodPtrs>...>;
    };

    /// @brief Compile-time action_id lookup by method via variadic expansion
    namespace detail {
        // Helper for comparing a single method (only works if types match)
        template<auto SearchPtr, auto CurrentPtr>
        struct is_same_method_ptr : std::false_type {};

        // Specialization for identical types - we can compare values
        template<auto Ptr>
        struct is_same_method_ptr<Ptr, Ptr> : std::true_type {};

        // Compile-time search via constexpr loop
        template<auto SearchPtr, auto... MethodPtrs>
        static constexpr uint64_t find_method_index() {
            // Create match array via pack expansion
            constexpr bool matches[] = {is_same_method_ptr<SearchPtr, MethodPtrs>::value...};

            // Linear search for first match
            for (std::size_t i = 0; i < sizeof...(MethodPtrs); ++i) {
                if (matches[i]) {
                    return static_cast<uint64_t>(i);
                }
            }
            return 0; // Not found
        }
    }

    /// @brief Get action_id for method from Actor::dispatch_traits (compile-time)
    /// ActionId is generated automatically based on position in the list (0, 1, 2, ...)
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
    /// // Usage:
    /// case msg_id<MyActor, &MyActor::insert>:
    /// @endcode
    template<typename Actor, auto MethodPtr, typename MethodList>
    struct action_id_impl;

    // Specialization for type_list with extraction of all MethodPtrs
    template<typename Actor, auto SearchPtr, auto... MethodPtrs>
    struct action_id_impl<Actor, SearchPtr, type_traits::type_list<method_map_entry<MethodPtrs>...>> {
        static constexpr uint64_t value = detail::find_method_index<SearchPtr, MethodPtrs...>();
    };

    /// @brief Compile-time constexpr variable for message_id
    /// Combines make_message_id + action_id for maximally compact syntax
    template<typename Actor, auto MethodPtr>
    inline constexpr auto msg_id = mailbox::make_message_id(
        action_id_impl<Actor, MethodPtr, typename Actor::dispatch_traits::methods>::value
    );

    /// @brief Runtime method lookup and message sending
    template<typename Actor, typename Method, typename MethodList>
    struct runtime_dispatch_helper;

    // Base case - method not found
    template<typename Actor, typename Method>
    struct runtime_dispatch_helper<Actor, Method, type_traits::type_list<>> {
        template<typename ActorPtr, typename Sender, typename... Args>
        static bool dispatch(Method method, ActorPtr* actor, Sender sender, Args&&... args) {
            (void)method; (void)actor; (void)sender; (void)sizeof...(args);
            return false; // Method not found
        }
    };

    // Forward declaration for detail::dispatch_method_impl
    namespace detail {
        template<typename Actor, auto MethodPtr, uint64_t ActionId, typename ActorPtr, typename Sender, typename... Args>
        void dispatch_method_impl(ActorPtr* actor, Sender sender, Args&&... args);
    }

    // Non-recursive helper for dispatching a single method
    template<typename Actor, typename Method, auto MethodPtr, uint64_t Index, typename ActorPtr, typename Sender, typename... Args>
    static bool try_dispatch_one(Method method, ActorPtr* actor, Sender sender, Args&&... args) {
        // Compare only if signatures match
        if constexpr (std::is_same<Method, decltype(MethodPtr)>::value) {
            if (method == MethodPtr) {
                // Found! Send message with ActionId = Index
                detail::dispatch_method_impl<Actor, MethodPtr, Index>(
                    actor, sender, std::forward<Args>(args)...);
                return true;
            }
        }
        return false;
    }

    // Non-recursive implementation via fold expression
    template<typename Actor, typename Method, auto... MethodPtrs, typename ActorPtr, typename Sender, typename... Args, std::size_t... Is>
    static bool dispatch_impl(Method method, ActorPtr* actor, Sender sender, std::index_sequence<Is...>, Args&&... args) {
        // Use fold expression with || operator for short-circuit evaluation
        return (try_dispatch_one<Actor, Method, MethodPtrs, Is>(method, actor, sender, std::forward<Args>(args)...) || ...);
    }

    // Specialization of runtime_dispatch_helper with extraction of MethodPtrs from type_list
    template<typename Actor, typename Method, auto... MethodPtrs>
    struct runtime_dispatch_helper<Actor, Method, type_traits::type_list<method_map_entry<MethodPtrs>...>> {
        template<typename ActorPtr, typename Sender, typename... Args>
        static bool dispatch(Method method, ActorPtr* actor, Sender sender, Args&&... args) {
            return dispatch_impl<Actor, Method, MethodPtrs...>(
                method, actor, sender,
                std::index_sequence_for<method_map_entry<MethodPtrs>...>{},
                std::forward<Args>(args)...);
        }
    };

} // namespace actor_zeta