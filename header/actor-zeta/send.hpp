#pragma once

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/base/dispatch_traits.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/make_message.hpp>
#include <actor-zeta/mailbox/id.hpp>

namespace actor_zeta {

namespace detail {

    // =========================================================================
    // Compile-time argument validation for send()
    // =========================================================================

    template<typename Actor, auto MethodPtr, uint64_t ActionId, typename ActorPtr, typename Sender, typename... Args>
    inline auto dispatch_method_impl(ActorPtr* actor, Sender sender, Args&&... args)
        -> typename Actor::template unique_future<
            unwrap_future_t<typename type_traits::callable_trait<decltype(MethodPtr)>::result_type>>
    {
        using callable_trait = type_traits::callable_trait<decltype(MethodPtr)>;
        using method_result_type = typename callable_trait::result_type;
        using expected_args = typename callable_trait::args_types;
        using provided_args = type_traits::type_list<Args...>;

        // =====================================================================
        // VALIDATION 1: Argument count must match
        // =====================================================================
        constexpr size_t expected_count = callable_trait::number_of_arguments;
        constexpr size_t provided_count = sizeof...(Args);

        static_assert(
            expected_count == provided_count,
            "send(): argument count mismatch - check method signature"
        );

        // =====================================================================
        // VALIDATION 2: Argument types must be compatible (convertible)
        // =====================================================================
        static_assert(
            args_compatible_v<expected_args, provided_args>,
            "send(): argument types are not compatible with method signature"
        );

        // =====================================================================
        // VALIDATION 3: All arguments must be storable in RTT
        // (move/copy constructible, not abstract, not reference)
        // =====================================================================
        static_assert(
            all_args_storable_v<Args...>,
            "send(): all arguments must be storable in message "
            "(move/copy constructible, not abstract)"
        );

        // Unwrap unique_future<T> returns to T for handler integration
        using actual_result_type = unwrap_future_t<method_result_type>;

        Actor* typed_actor;
        if constexpr (std::is_same_v<ActorPtr, base::address_t>) {
            typed_actor = static_cast<Actor*>(actor->operator->());
        } else {
            typed_actor = actor;
        }

        return typed_actor->template enqueue_impl<actual_result_type>(
            sender,
            mailbox::make_message_id(ActionId),
            std::forward<Args>(args)...
        );
    }
} // namespace detail

    template<typename ActorPtr, typename Sender, typename Method, typename... Args,
             typename Actor = typename type_traits::callable_trait<Method>::class_type>
    [[nodiscard]] inline auto send(ActorPtr* actor, Sender sender, Method method, Args&&... args)
        -> typename Actor::template unique_future<
            detail::unwrap_future_t<typename type_traits::callable_trait<Method>::result_type>>
    {
        using result_type = typename type_traits::callable_trait<Method>::result_type;

        static_assert(!std::is_same_v<result_type, bool>,
            "Actor methods must not return bool. "
            "Use void or other types (int, std::string, etc.) - all return results via promise/future.");

        using methods = typename Actor::dispatch_traits::methods;

        return runtime_dispatch_helper<Actor, Method, methods>::dispatch(
            method, actor, sender, std::forward<Args>(args)...);
    }

    template<typename Sender, typename Method, typename... Args,
             typename Actor = typename type_traits::callable_trait<Method>::class_type>
    [[nodiscard]] inline auto send(base::address_t target, Sender sender, Method method, Args&&... args)
        -> typename Actor::template unique_future<
            detail::unwrap_future_t<typename type_traits::callable_trait<Method>::result_type>>
    {
        using result_type = typename type_traits::callable_trait<Method>::result_type;

        static_assert(!std::is_same_v<result_type, bool>,
            "Actor methods must not return bool. "
            "Use void or other types (int, std::string, etc.) - all return results via promise/future.");

        assert(target && "target address must not be empty");

        using methods = typename Actor::dispatch_traits::methods;

        return runtime_dispatch_helper<Actor, Method, methods>::dispatch(
            method, target, sender, std::forward<Args>(args)...);
    }

} // namespace actor_zeta