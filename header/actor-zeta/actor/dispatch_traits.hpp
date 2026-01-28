#pragma once

#include <concepts>
#include <cstdint>

#include <actor-zeta/actor/forwards.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/forwards.hpp>
#include <actor-zeta/detail/ignore_unused.hpp>
#include <actor-zeta/mailbox/id.hpp>

namespace actor_zeta {

    // Method map entry

    template<auto MethodPtr>
    struct method_map_entry {};

    template<auto MethodPtr>
    using method = method_map_entry<MethodPtr>;

    namespace detail {

        template<typename T>
        concept const_lvalue_ref = std::is_lvalue_reference_v<T> && std::is_const_v<std::remove_reference_t<T>>;

        template<typename... Args>
        concept has_any_const_lvalue_ref = (const_lvalue_ref<Args> || ...);

        // Detects T&& to move-only type. Use T instead - see docs/GCC_COROUTINE_OPERATOR_NEW_BUG.md
        template<typename T>
        concept rvalue_ref_to_move_only =
            std::is_rvalue_reference_v<T> &&
            std::is_move_constructible_v<std::remove_reference_t<T>> &&
            !std::is_copy_constructible_v<std::remove_reference_t<T>>;

        template<typename... Args>
        concept has_any_rvalue_ref_to_move_only = (rvalue_ref_to_move_only<Args> || ...);

        namespace type_list_check {
            template<typename ArgsList>
            struct has_const_ref_impl;

            template<typename... Args>
            struct has_const_ref_impl<type_traits::type_list<Args...>> {
                static constexpr bool value = has_any_const_lvalue_ref<Args...>;
            };

            template<typename ArgsList>
            struct has_rvalue_ref_move_only_impl;

            template<typename... Args>
            struct has_rvalue_ref_move_only_impl<type_traits::type_list<Args...>> {
                static constexpr bool value = has_any_rvalue_ref_to_move_only<Args...>;
            };
        }

        template<typename ArgsList>
        concept type_list_has_const_lvalue_ref = type_list_check::has_const_ref_impl<ArgsList>::value;

        template<typename ArgsList>
        concept type_list_has_rvalue_ref_move_only = type_list_check::has_rvalue_ref_move_only_impl<ArgsList>::value;

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

            static constexpr bool is_coroutine =
                type_traits::is_unique_future_v<result_type> ||
                type_traits::is_generator_v<result_type>;
            static constexpr bool has_const_ref = type_list_has_const_lvalue_ref<args_types>;
            static constexpr bool has_rvalue_ref_move_only = type_list_has_rvalue_ref_move_only<args_types>;

            // Safe if: not a coroutine, or (no const& and no T&& to move-only)
            static constexpr bool no_const_ref = !is_coroutine || !has_const_ref;
            static constexpr bool no_rvalue_move_only = !is_coroutine || !has_rvalue_ref_move_only;
        };

        // Actor/Interface detection concepts

        template<typename T>
        concept has_dispatch_traits = requires {
            typename T::dispatch_traits;
            typename T::dispatch_traits::methods;
        };

        /// Detects cooperative_actor via marker type (mailbox() is protected)
        template<typename T>
        concept is_cooperative_actor = requires {
            typename T::is_cooperative_actor_type;
        };

        /// Actor: has dispatch_traits and is cooperative_actor (concrete implementation)
        template<typename T>
        concept is_actor = has_dispatch_traits<T> && is_cooperative_actor<T>;

        /// Interface: has dispatch_traits but is NOT an actor (pure contract)
        template<typename T>
        concept is_interface = has_dispatch_traits<T> && !is_cooperative_actor<T>;

    } // namespace detail

    // dispatch_traits implementation

    namespace detail {
        // Helper to extract methods from variadic pack
        template<auto... MethodPtrs>
        struct dispatch_traits_parser {
            using methods = type_traits::type_list<method_map_entry<MethodPtrs>...>;

            static constexpr bool all_valid =
                (method_return_type_check<MethodPtrs>::is_valid && ...);
            static constexpr bool all_no_const_ref =
                (coroutine_parameter_check<MethodPtrs>::no_const_ref && ...);
            static constexpr bool all_no_rvalue_move_only =
                (coroutine_parameter_check<MethodPtrs>::no_rvalue_move_only && ...);
        };

        template<auto First>
        struct dispatch_traits_parser<First> {
            using methods = type_traits::type_list<method_map_entry<First>>;

            static constexpr bool all_valid = method_return_type_check<First>::is_valid;
            static constexpr bool all_no_const_ref = coroutine_parameter_check<First>::no_const_ref;
            static constexpr bool all_no_rvalue_move_only = coroutine_parameter_check<First>::no_rvalue_move_only;
        };
    } // namespace detail

    /// Dispatch traits - list of method pointers for an actor/interface.
    /// Usage:
    ///   dispatch_traits<&Actor::method1, &Actor::method2>
    template<auto... Args>
    struct dispatch_traits {
    private:
        using parser = detail::dispatch_traits_parser<Args...>;

    public:
        using methods = typename parser::methods;

        static_assert(
            parser::all_valid,
            "All actor methods must return unique_future<T> or generator<T>. "
            "Raw void or value returns are not allowed. "
            "All actor methods must be coroutines using co_return or co_yield.");

        static_assert(
            parser::all_no_const_ref,
            "Coroutine methods must not have const& parameters. "
            "After co_await, message is destroyed and const& becomes dangling. Use by-value instead.");

        static_assert(
            parser::all_no_rvalue_move_only,
            "Coroutine methods must not have T&& parameters for move-only types (e.g., std::unique_ptr<T>&&). "
            "GCC 11.4 has a bug where operator new doesn't receive arguments for such signatures. "
            "Use by-value instead: T (not T&&). See docs/GCC_COROUTINE_OPERATOR_NEW_BUG.md");
    };

    // Empty dispatch_traits (no methods)
    template<>
    struct dispatch_traits<> {
        using methods = type_traits::type_list<>;
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

        // send_result_t - wraps unique_future in pair<bool, future> for needs_scheduling
        template<typename Actor, typename ResultType>
        struct send_result_type {
            using future_type = dispatch_result_t<Actor, ResultType>;
            using type = std::pair<bool, future_type>;
        };

        // Generators also return pair<bool, generator<T>> for consistency
        template<typename Actor, typename T>
        struct send_result_type<Actor, generator<T>> {
            using type = std::pair<bool, generator<T>>;
        };

        template<typename Actor, typename ResultType>
        using send_result_t = typename send_result_type<Actor, ResultType>::type;

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

        // =======================================================================
        // Compile-time method validation helpers
        // =======================================================================

        /// Check if method signature (type) exists in method list
        /// This catches errors when method with wrong signature is passed to send()
        template<typename Method, typename MethodList>
        struct method_signature_exists;

        template<typename Method>
        struct method_signature_exists<Method, type_traits::type_list<>> {
            static constexpr bool value = false;
        };

        template<typename Method, auto FirstPtr, auto... RestPtrs>
        struct method_signature_exists<Method, type_traits::type_list<method_map_entry<FirstPtr>, method_map_entry<RestPtrs>...>> {
            static constexpr bool value =
                std::is_same_v<Method, decltype(FirstPtr)> ||
                method_signature_exists<Method, type_traits::type_list<method_map_entry<RestPtrs>...>>::value;
        };

        template<typename Method, typename MethodList>
        inline constexpr bool method_signature_exists_v = method_signature_exists<Method, MethodList>::value;

        /// Check that method belongs to the expected class
        template<typename Method, typename ExpectedClass>
        struct method_belongs_to_class {
            using actual_class = typename type_traits::callable_trait<Method>::class_type;
            static constexpr bool value = std::is_same_v<actual_class, ExpectedClass> ||
                                          std::is_base_of_v<actual_class, ExpectedClass>;
        };

        template<typename Method, typename ExpectedClass>
        inline constexpr bool method_belongs_to_class_v = method_belongs_to_class<Method, ExpectedClass>::value;

        /// Combined validation for send()
        template<typename Actor, typename Method>
        struct validate_method_for_send {
            using methods = typename Actor::dispatch_traits::methods;
            using method_class = typename type_traits::callable_trait<Method>::class_type;

            static_assert(
                method_signature_exists_v<Method, methods>,
                "send(): Method signature not found in Actor::dispatch_traits. "
                "Ensure the method is registered: using dispatch_traits = actor_zeta::dispatch_traits<&Actor::method, ...>;");

            static_assert(
                method_belongs_to_class_v<Method, Actor>,
                "send(): Method does not belong to this Actor class. "
                "Check that you're calling the correct actor's method.");

            static constexpr bool valid = method_signature_exists_v<Method, methods> &&
                                          method_belongs_to_class_v<Method, Actor>;
        };

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
        template<typename Actor, auto MethodPtr, uint64_t ActionId, typename ActorPtr, typename... Args>
        auto dispatch_method_impl(ActorPtr* actor, Args&&... args)
            -> send_result_t<Actor, typename type_traits::callable_trait<decltype(MethodPtr)>::result_type>;

        template<typename Interface, auto MethodPtr, uint64_t ActionId, typename... Args>
        auto dispatch_method_impl_address(actor::address_t target, Args&&... args)
            -> send_result_t<Interface, typename type_traits::callable_trait<decltype(MethodPtr)>::result_type>;
    }

    template<typename Actor, typename Method>
    struct runtime_dispatch_helper<Actor, Method, type_traits::type_list<>> {
        template<typename ActorPtr, typename... Args>
        static auto dispatch(Method method, ActorPtr* actor, Args&&... args)
            -> detail::send_result_t<Actor, typename type_traits::callable_trait<Method>::result_type> {
            detail::ignore_unused(method, actor, sizeof...(args));
            assert(false && "Method not found in dispatch_traits");
            std::abort();
        }
    };

    template<typename Actor, typename Method, typename ActorPtr, typename... Args>
    struct dispatch_one_impl {
        using result_type = typename type_traits::callable_trait<Method>::result_type;
        using dispatch_return_type = detail::send_result_t<Actor, result_type>;

        template<std::size_t... Is>
        static auto dispatch(Method method, ActorPtr* actor, std::index_sequence<>, Args&&... args)
            -> dispatch_return_type {
            detail::ignore_unused(method, actor, sizeof...(args));
            assert(false && "Method not found in dispatch_traits");
            std::abort();
        }

        template<auto FirstMethod, auto... RestMethods, std::size_t FirstIndex, std::size_t... RestIndices>
        static auto dispatch(Method method, ActorPtr* actor,
                             std::index_sequence<FirstIndex, RestIndices...>, Args&&... args)
            -> dispatch_return_type {
            if constexpr (std::same_as<Method, decltype(FirstMethod)>) {
                if (method == FirstMethod) {
                    return detail::dispatch_method_impl<Actor, FirstMethod, FirstIndex>(
                        actor, std::forward<Args>(args)...);
                }
            }

            return dispatch<RestMethods...>(
                method, actor,
                std::index_sequence<RestIndices...>{},
                std::forward<Args>(args)...);
        }
    };

    template<typename Actor, typename Method, auto... MethodPtrs, typename ActorPtr, typename... Args, std::size_t... Is>
    static auto dispatch_impl(Method method, ActorPtr* actor, std::index_sequence<Is...>, Args&&... args)
        -> detail::send_result_t<Actor, typename type_traits::callable_trait<Method>::result_type> {
        return dispatch_one_impl<Actor, Method, ActorPtr, Args...>::template dispatch<MethodPtrs...>(
            method, actor,
            std::index_sequence<Is...>{},
            std::forward<Args>(args)...);
    }

    template<typename Actor, typename Method, auto... MethodPtrs>
    struct runtime_dispatch_helper<Actor, Method, type_traits::type_list<method_map_entry<MethodPtrs>...>> {
        using result_type = typename type_traits::callable_trait<Method>::result_type;
        using dispatch_return_type = detail::send_result_t<Actor, result_type>;

        template<typename ActorPtr, typename... Args>
        static auto dispatch(Method method, ActorPtr* actor, Args&&... args)
            -> dispatch_return_type {
            return dispatch_impl<Actor, Method, MethodPtrs...>(
                method, actor,
                std::index_sequence_for<method_map_entry<MethodPtrs>...>{},
                std::forward<Args>(args)...);
        }

        template<typename... Args>
        static auto dispatch(Method method, actor::address_t target, Args&&... args)
            -> dispatch_return_type {
            auto* actor = static_cast<Actor*>(target.get());
            return dispatch_impl<Actor, Method, MethodPtrs...>(
                method, actor,
                std::index_sequence_for<method_map_entry<MethodPtrs>...>{},
                std::forward<Args>(args)...);
        }
    };

    // runtime_dispatch_helper_address - for interface polymorphism via address_t

    template<typename Interface, typename Method, typename MethodList>
    struct runtime_dispatch_helper_address;

    template<typename Interface, typename Method>
    struct runtime_dispatch_helper_address<Interface, Method, type_traits::type_list<>> {
        template<typename... Args>
        static auto dispatch(Method method, actor::address_t target, Args&&... args)
            -> detail::send_result_t<Interface, typename type_traits::callable_trait<Method>::result_type> {
            detail::ignore_unused(method, target, sizeof...(args));
            assert(false && "Method not found in dispatch_traits");
            std::abort();
        }
    };

    template<typename Interface, typename Method, typename... Args>
    struct dispatch_one_impl_address {
        using result_type = typename type_traits::callable_trait<Method>::result_type;
        using dispatch_return_type = detail::send_result_t<Interface, result_type>;

        template<std::size_t... Is>
        static auto dispatch(Method method, actor::address_t target, std::index_sequence<>, Args&&... args)
            -> dispatch_return_type {
            detail::ignore_unused(method, target, sizeof...(args));
            assert(false && "Method not found in dispatch_traits");
            std::abort();
        }

        template<auto FirstMethod, auto... RestMethods, std::size_t FirstIndex, std::size_t... RestIndices>
        static auto dispatch(Method method, actor::address_t target,
                             std::index_sequence<FirstIndex, RestIndices...>, Args&&... args)
            -> dispatch_return_type {
            if constexpr (std::same_as<Method, decltype(FirstMethod)>) {
                if (method == FirstMethod) {
                    return detail::dispatch_method_impl_address<Interface, FirstMethod, FirstIndex>(
                        target, std::forward<Args>(args)...);
                }
            }

            return dispatch<RestMethods...>(
                method, target,
                std::index_sequence<RestIndices...>{},
                std::forward<Args>(args)...);
        }
    };

    template<typename Interface, typename Method, auto... MethodPtrs, typename... Args, std::size_t... Is>
    static auto dispatch_impl_address(Method method, actor::address_t target, std::index_sequence<Is...>, Args&&... args)
        -> detail::send_result_t<Interface, typename type_traits::callable_trait<Method>::result_type> {
        return dispatch_one_impl_address<Interface, Method, Args...>::template dispatch<MethodPtrs...>(
            method, target,
            std::index_sequence<Is...>{},
            std::forward<Args>(args)...);
    }

    template<typename Interface, typename Method, auto... MethodPtrs>
    struct runtime_dispatch_helper_address<Interface, Method, type_traits::type_list<method_map_entry<MethodPtrs>...>> {
        using result_type = typename type_traits::callable_trait<Method>::result_type;
        using dispatch_return_type = detail::send_result_t<Interface, result_type>;

        template<typename... Args>
        static auto dispatch(Method method, actor::address_t target, Args&&... args)
            -> dispatch_return_type {
            return dispatch_impl_address<Interface, Method, MethodPtrs...>(
                method, target,
                std::index_sequence_for<method_map_entry<MethodPtrs>...>{},
                std::forward<Args>(args)...);
        }
    };

} // namespace actor_zeta