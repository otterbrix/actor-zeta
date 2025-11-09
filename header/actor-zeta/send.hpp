#pragma once

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/base/dispatch_traits.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/make_message.hpp>
#include <actor-zeta/mailbox/id.hpp>

namespace actor_zeta {

namespace detail {
    /// @brief Helper function for sending messages
    /// @tparam Actor Actor type
    /// @tparam MethodPtr Method pointer
    /// @tparam ActionId Action ID for the method
    template<typename Actor, auto MethodPtr, uint64_t ActionId, typename ActorPtr, typename Sender, typename... Args>
    inline void dispatch_method_impl(ActorPtr* actor, Sender sender, Args&&... args) {
        actor->enqueue(
            make_message(
                actor->resource(),
                sender,
                mailbox::make_message_id(ActionId),
                std::forward<Args>(args)...
            )
        );
    }
} // namespace detail

    /// @brief Send function WITHOUT macros - accepts runtime method pointer
    /// Automatically searches for action_id in Actor::dispatch_traits
    /// ActionId is generated automatically based on position in the list (0, 1, 2, ...)
    ///
    /// Usage example:
    /// @code
    /// class MyActor {
    ///     struct dispatch_traits {
    ///         using methods = type_list<
    ///             method<&MyActor::insert>,    // ActionId = 0
    ///             method<&MyActor::remove>     // ActionId = 1
    ///         >;
    ///     };
    /// };
    /// send(actor, sender, &MyActor::insert, key, value);
    /// @endcode
    template<typename ActorPtr, typename Sender, typename Method, typename... Args>
    inline auto send(ActorPtr* actor, Sender sender, Method method, Args&&... args)
        -> std::enable_if_t<std::is_member_function_pointer<Method>::value, void>
    {
        using Actor = typename type_traits::callable_trait<Method>::class_type;
        using methods = typename Actor::dispatch_traits::methods;

        // Runtime method lookup and dispatch
        bool found = runtime_dispatch_helper<Actor, Method, methods>::dispatch(
            method, actor, sender, std::forward<Args>(args)...);

        assert(found && "Method not found in dispatch_traits");
    }

} // namespace actor_zeta
