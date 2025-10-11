#pragma once

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/base/dispatch_traits.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/make_message.hpp>
#include <actor-zeta/mailbox/id.hpp>

namespace actor_zeta {

namespace detail {
    /// @brief Вспомогательная функция для отправки сообщений
    /// @tparam Actor Тип актора
    /// @tparam MethodPtr Указатель на метод
    /// @tparam ActionId Action ID для метода
    template<typename Actor, auto MethodPtr, uint64_t ActionId, typename ActorPtr, typename Sender, typename... Args>
    inline void dispatch_method_impl(ActorPtr* actor, Sender sender, Args&&... args) {
        auto msg_id = mailbox::make_message_id(ActionId);
        actor->enqueue(
            make_message(
                actor->resource(),
                sender,
                msg_id,
                std::forward<Args>(args)...
            )
        );
    }
} // namespace detail

    /// @brief Функция send БЕЗ макроса - принимает runtime указатель на метод
    /// Автоматически ищет action_id в Actor::dispatch_traits
    /// ActionId генерируется автоматически на основе позиции в списке (0, 1, 2, ...)
    ///
    /// Пример использования:
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

        // Runtime поиск метода и отправка
        bool found = runtime_dispatch_helper<Actor, Method, methods>::dispatch(
            method, actor, sender, std::forward<Args>(args)...);

        assert(found && "Method not found in dispatch_traits");
    }

} // namespace actor_zeta
