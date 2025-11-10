#pragma once

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/base/dispatch_traits.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/make_message.hpp>
#include <actor-zeta/mailbox/id.hpp>

namespace actor_zeta {

namespace detail {
    template<typename Actor, auto MethodPtr, uint64_t ActionId, typename ActorPtr, typename Sender, typename... Args>
    inline bool dispatch_method_impl(ActorPtr* actor, Sender sender, Args&&... args) {
        return actor->enqueue(
            make_message(
                actor->resource(),
                sender,
                mailbox::make_message_id(ActionId),
                std::forward<Args>(args)...
            )
        );
    }
} // namespace detail


    template<typename ActorPtr, typename Sender, typename Method, typename... Args>
    [[nodiscard]] inline auto send(ActorPtr* actor, Sender sender, Method method, Args&&... args)
        -> std::enable_if_t<std::is_member_function_pointer<Method>::value, bool>
    {
        using Actor = typename type_traits::callable_trait<Method>::class_type;
        using methods = typename Actor::dispatch_traits::methods;


        return runtime_dispatch_helper<Actor, Method, methods>::dispatch(
            method, actor, sender, std::forward<Args>(args)...);
    }

    template<typename Sender, typename Method, typename... Args>
    [[nodiscard]] inline auto send(base::address_t target, Sender sender, Method method, Args&&... args)
        -> std::enable_if_t<std::is_member_function_pointer<Method>::value, bool>
    {
        assert(target && "target address must not be empty");

        using Actor = typename type_traits::callable_trait<Method>::class_type;
        using methods = typename Actor::dispatch_traits::methods;


        return runtime_dispatch_helper<Actor, Method, methods>::dispatch(
            method, target, sender, std::forward<Args>(args)...);
    }

} // namespace actor_zeta
