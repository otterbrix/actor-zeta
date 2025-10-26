#pragma once

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/base/dispatch_traits.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/make_message.hpp>
#include <actor-zeta/mailbox/id.hpp>

namespace actor_zeta {

namespace detail {
    template<typename Actor, auto MethodPtr, uint64_t ActionId, typename ActorPtr, typename Sender, typename... Args>
    inline auto dispatch_method_impl(ActorPtr* actor, Sender sender, Args&&... args)
        -> typename Actor::template unique_future<
            typename type_traits::callable_trait<decltype(MethodPtr)>::result_type>
    {
        using callable_trait = type_traits::callable_trait<decltype(MethodPtr)>;
        using result_type = typename callable_trait::result_type;

        base::actor_abstract_t* base_ptr;
        if constexpr (std::is_same_v<ActorPtr, base::address_t>) {
            base_ptr = actor->operator->();
        } else {
            base_ptr = actor;
        }

        auto msg = detail::make_message(
            base_ptr->resource(),
            sender,
            mailbox::make_message_id(ActionId),
            std::forward<Args>(args)...);

        auto* typed_actor = static_cast<Actor*>(base_ptr);
        return typed_actor->template enqueue_impl<result_type>(std::move(msg));
    }
} // namespace detail

    template<typename ActorPtr, typename Sender, typename Method, typename... Args,
             typename Actor = typename type_traits::callable_trait<Method>::class_type>
    inline auto send(ActorPtr* actor, Sender sender, Method method, Args&&... args)
        -> typename Actor::template unique_future<
            typename type_traits::callable_trait<Method>::result_type>
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
    inline auto send(base::address_t target, Sender sender, Method method, Args&&... args)
        -> typename Actor::template unique_future<
            typename type_traits::callable_trait<Method>::result_type>
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