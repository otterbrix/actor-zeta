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
    /// @return true if actor was unblocked and needs scheduling, false otherwise
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

    /// @brief Функция send БЕЗ макроса - принимает runtime указатель на метод
    /// Автоматически ищет action_id в Actor::dispatch_traits
    /// ActionId генерируется автоматически на основе позиции в списке (0, 1, 2, ...)
    ///
    /// @return true if actor was unblocked and needs scheduling, false otherwise
    ///
    /// IMPORTANT: Return value must be checked! Use it to conditionally schedule the actor:
    ///   - true:  actor was blocked and now unblocked → must call scheduler->enqueue(actor)
    ///   - false: actor is already active or queue closed → do NOT schedule
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
    /// if (send(actor, sender, &MyActor::insert, key, value)) {
    ///     scheduler->enqueue(actor);
    /// }
    /// @endcode
    template<typename ActorPtr, typename Sender, typename Method, typename... Args>
    [[nodiscard]] inline auto send(ActorPtr* actor, Sender sender, Method method, Args&&... args)
        -> std::enable_if_t<std::is_member_function_pointer<Method>::value, bool>
    {
        using Actor = typename type_traits::callable_trait<Method>::class_type;
        using methods = typename Actor::dispatch_traits::methods;

        // Runtime поиск метода и отправка
        // Returns: true if actor was unblocked (needs scheduling), false otherwise
        return runtime_dispatch_helper<Actor, Method, methods>::dispatch(
            method, actor, sender, std::forward<Args>(args)...);
    }

    /// @brief Функция send для отправки по address_t с указателем на метод
    /// Автоматически ищет action_id в Actor::dispatch_traits
    /// ActionId генерируется автоматически на основе позиции в списке (0, 1, 2, ...)
    ///
    /// @return true if actor was unblocked and needs scheduling, false otherwise
    ///
    /// IMPORTANT: Return value must be checked! Use it to conditionally schedule the actor:
    ///   - true:  actor was blocked and now unblocked → must call scheduler->enqueue(actor)
    ///   - false: actor is already active or queue closed → do NOT schedule
    ///
    /// Пример использования:
    /// @code
    /// if (send(actor_address, sender_address, &MyActor::insert, key, value)) {
    ///     scheduler->enqueue(actor_address);
    /// }
    /// @endcode
    template<typename Sender, typename Method, typename... Args>
    [[nodiscard]] inline auto send(base::address_t target, Sender sender, Method method, Args&&... args)
        -> std::enable_if_t<std::is_member_function_pointer<Method>::value, bool>
    {
        assert(target && "target address must not be empty");

        using Actor = typename type_traits::callable_trait<Method>::class_type;
        using methods = typename Actor::dispatch_traits::methods;

        // Runtime поиск метода и отправка
        // Returns: true if actor was unblocked (needs scheduling), false otherwise
        return runtime_dispatch_helper<Actor, Method, methods>::dispatch(
            method, target, sender, std::forward<Args>(args)...);
    }

} // namespace actor_zeta
