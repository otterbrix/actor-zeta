#pragma once

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/base/dispatch_traits.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/make_message.hpp>
#include <actor-zeta/mailbox/id.hpp>

namespace actor_zeta {

namespace detail {
    // Helper: unwrap unique_future<T> to T for handler integration
    template<typename T>
    struct unwrap_future {
        using type = T;
    };

    template<typename T>
    struct unwrap_future<type_traits::is_unique_future<T>> {
        using type = typename type_traits::is_unique_future<T>::value_type;
    };

    template<typename T>
    using unwrap_future_t = typename std::conditional_t<
        type_traits::is_unique_future_v<T>,
        typename type_traits::is_unique_future<T>::value_type,
        T
    >;

    template<typename Actor, auto MethodPtr, uint64_t ActionId, typename ActorPtr, typename Sender, typename... Args>
    inline auto dispatch_method_impl(ActorPtr* actor, Sender sender, Args&&... args)
        -> typename Actor::template unique_future<
            unwrap_future_t<typename type_traits::callable_trait<decltype(MethodPtr)>::result_type>>
    {
        using callable_trait = type_traits::callable_trait<decltype(MethodPtr)>;
        using method_result_type = typename callable_trait::result_type;

        // PHASE 3: Unwrap unique_future<T> returns to T for handler integration
        // When method returns unique_future<T>, handler calls get() and stores T in result_slot
        // So send() should create future<T>, not future<unique_future<T>>
        using actual_result_type = unwrap_future_t<method_result_type>;

        Actor* typed_actor;
        if constexpr (std::is_same_v<ActorPtr, base::address_t>) {
            // address_t provides operator->() which returns the underlying actor pointer
            typed_actor = static_cast<Actor*>(actor->operator->());
        } else {
            // Direct actor pointer
            typed_actor = actor;
        }

        auto msg = detail::make_message(
            typed_actor->resource(),
            sender,
            mailbox::make_message_id(ActionId),
            std::forward<Args>(args)...);

        return typed_actor->template enqueue_impl<actual_result_type>(std::move(msg));
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