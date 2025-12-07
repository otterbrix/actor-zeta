#pragma once

#include <cstdint>

#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/type_list.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/mailbox/id.hpp>

namespace actor_zeta {

    /// @brief Compile-time map entry: (MethodPtr -> ActionId)
    template<auto MethodPtr>
    struct method_map_entry {};

    template<auto MethodPtr>
    using method = method_map_entry<MethodPtr>;

    namespace detail {
        // =========================================================================
        // Compile-time safety: detect const& parameters in coroutines
        // Coroutines MUST NOT have const& parameters - they become dangling after co_await
        // =========================================================================

        /// @brief Detect if T is const lvalue reference (const U&)
        template<typename T>
        struct is_const_lvalue_ref : std::false_type {};

        template<typename T>
        struct is_const_lvalue_ref<const T&> : std::true_type {};

        /// @brief Check if any type in parameter pack is const lvalue ref
        template<typename... Args>
        struct has_const_lvalue_ref_args : std::disjunction<is_const_lvalue_ref<Args>...> {};

        /// @brief Check if any type in type_list is const lvalue ref
        template<typename ArgsList>
        struct has_const_lvalue_ref_in_list;

        template<typename... Args>
        struct has_const_lvalue_ref_in_list<type_traits::type_list<Args...>>
            : has_const_lvalue_ref_args<Args...> {};

        /// @brief Compile-time check: coroutines must not have const& parameters
        /// @tparam MethodPtr Pointer to member function
        /// After co_await, the message is destroyed, making const& parameters dangling.
        /// This causes use-after-free bugs that are hard to debug.
        template<auto MethodPtr>
        struct coroutine_parameter_check {
            using trait = type_traits::callable_trait<decltype(MethodPtr)>;
            using result_type = typename trait::result_type;
            using args_types = typename trait::args_types;

            static constexpr bool is_coroutine = type_traits::is_unique_future_v<result_type>;
            static constexpr bool has_const_ref = has_const_lvalue_ref_in_list<args_types>::value;

            // Coroutines MUST NOT have const& parameters (use-after-free after co_await)
            static constexpr bool is_safe = !is_coroutine || !has_const_ref;
        };
    } // namespace detail

    /// @brief Dispatch traits for actor methods
    /// @tparam MethodPtrs Pointers to member functions
    ///
    /// IMPORTANT: Coroutine methods (returning unique_future<T>) must NOT have const& parameters!
    /// After co_await, the message is destroyed and const& becomes a dangling reference.
    /// Use by-value parameters instead.
    ///
    /// @code
    /// // WRONG - const& in coroutine causes use-after-free:
    /// unique_future<int> process(const std::string& data);  // COMPILE ERROR!
    ///
    /// // CORRECT - by-value is safe:
    /// unique_future<int> process(std::string data);  // OK
    /// @endcode
    template<auto... MethodPtrs>
    struct dispatch_traits {
        using methods = type_traits::type_list<method_map_entry<MethodPtrs>...>;

        // Compile-time safety: coroutines must not have const& parameters
        static_assert(
            (detail::coroutine_parameter_check<MethodPtrs>::is_safe && ...),
            "Coroutine methods (returning unique_future<T>) must not have const& parameters. "
            "After co_await, message is destroyed and const& becomes dangling. Use by-value instead.");
    };

    namespace detail {
        // Helper: unwrap unique_future<T> to T (for handler integration)
        template<typename T>
        using unwrap_future_t = typename std::conditional_t<
            type_traits::is_unique_future_v<T>,
            typename type_traits::is_unique_future<T>::value_type,
            T>;

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
    } // namespace detail

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
        action_id_impl<Actor, MethodPtr, typename Actor::dispatch_traits::methods>::value);

    template<typename Actor, typename Method, typename MethodList>
    struct runtime_dispatch_helper;

    namespace detail {
        template<typename Actor, auto MethodPtr, uint64_t ActionId, typename ActorPtr, typename Sender, typename... Args>
        auto dispatch_method_impl(ActorPtr* actor, Sender sender, Args&&... args)
            -> typename Actor::template unique_future<
                unwrap_future_t<typename type_traits::callable_trait<decltype(MethodPtr)>::result_type>>;
    }

    template<typename Actor, typename Method>
    struct runtime_dispatch_helper<Actor, Method, type_traits::type_list<>> {
        template<typename ActorPtr, typename Sender, typename... Args>
        static auto dispatch(Method method, ActorPtr* actor, Sender sender, Args&&... args)
            -> typename Actor::template unique_future<void> {
            (void) method;
            (void) actor;
            (void) sender;
            (void) sizeof...(args);
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
                detail::unwrap_future_t<typename type_traits::callable_trait<Method>::result_type>> {
            using result_type = typename type_traits::callable_trait<Method>::result_type;
            (void) method;
            (void) actor;
            (void) sender;
            (void) sizeof...(args);
            // Method not found - this should never happen in correct code
            assert(false && "Method not found in dispatch_traits");
            std::abort(); // Unreachable, but satisfies return type
        }

        // Recursive case - try first method, if not found continue with rest
        template<auto FirstMethod, auto... RestMethods, std::size_t FirstIndex, std::size_t... RestIndices>
        static auto dispatch(Method method, ActorPtr* actor, Sender sender,
                             std::index_sequence<FirstIndex, RestIndices...>, Args&&... args)
            -> typename Actor::template unique_future<
                detail::unwrap_future_t<typename type_traits::callable_trait<Method>::result_type>> {
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
            detail::unwrap_future_t<typename type_traits::callable_trait<Method>::result_type>> {
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
                detail::unwrap_future_t<typename type_traits::callable_trait<Method>::result_type>> {
            return dispatch_impl<Actor, Method, MethodPtrs...>(
                method, actor, sender,
                std::index_sequence_for<method_map_entry<MethodPtrs>...>{},
                std::forward<Args>(args)...);
        }

        template<typename Sender, typename... Args>
        static auto dispatch(Method method, actor::address_t target, Sender sender, Args&&... args)
            -> typename Actor::template unique_future<
                detail::unwrap_future_t<typename type_traits::callable_trait<Method>::result_type>> {
            auto* actor = static_cast<Actor*>(target.get());
            return dispatch_impl<Actor, Method, MethodPtrs...>(
                method, actor, sender,
                std::index_sequence_for<method_map_entry<MethodPtrs>...>{},
                std::forward<Args>(args)...);
        }
    };

} // namespace actor_zeta