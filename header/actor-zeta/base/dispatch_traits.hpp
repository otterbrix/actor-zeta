#pragma once

#include <cstdint>
#include <actor-zeta/detail/type_list.hpp>
#include <actor-zeta/mailbox/id.hpp>

namespace actor_zeta {

    /// @brief Compile-time map entry: (MethodPtr -> ActionId)
    /// ActionId генерируется автоматически на основе позиции в списке
    template<auto MethodPtr>
    struct method_map_entry {};

    /// @brief Короткий алиас для method_map_entry
    template<auto MethodPtr>
    using method = method_map_entry<MethodPtr>;

    /// @brief Вспомогательный тип для поиска action_id по методу (с учетом индекса)
    template<auto MethodPtr, typename MethodList, uint64_t Index = 0>
    struct find_action_id_with_index;

    // Базовый случай - метод не найден
    template<auto MethodPtr, uint64_t Index>
    struct find_action_id_with_index<MethodPtr, type_traits::type_list<>, Index> {
        static constexpr uint64_t value = 0;
    };

    // Рекурсивный случай - нашли метод
    template<auto MethodPtr, typename... Rest, uint64_t Index>
    struct find_action_id_with_index<MethodPtr, type_traits::type_list<method_map_entry<MethodPtr>, Rest...>, Index> {
        static constexpr uint64_t value = Index;
    };

    // Рекурсивный случай - метод не совпадает, ищем дальше
    template<auto MethodPtr, auto OtherMethodPtr, typename... Rest, uint64_t Index>
    struct find_action_id_with_index<MethodPtr, type_traits::type_list<method_map_entry<OtherMethodPtr>, Rest...>, Index> {
        static constexpr uint64_t value = find_action_id_with_index<MethodPtr, type_traits::type_list<Rest...>, Index + 1>::value;
    };

    /// @brief Получение action_id для метода из Actor::dispatch_traits
    /// ActionId генерируется автоматически на основе позиции в списке (0, 1, 2, ...)
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
    /// // Использование:
    /// case msg_id<MyActor, &MyActor::insert>():
    /// @endcode
    template<typename Actor, auto MethodPtr>
    struct action_id {
        static constexpr uint64_t value = find_action_id_with_index<MethodPtr, typename Actor::dispatch_traits::methods, 0>::value;
    };

    /// @brief Короткий хелпер для создания message_id из action
    /// Объединяет make_message_id + action для более компактного синтаксиса
    template<typename Actor, auto MethodPtr>
    constexpr auto msg_id() noexcept {
        return mailbox::make_message_id(action_id<Actor, MethodPtr>::value);
    }

    /// @brief Runtime поиск метода и отправка сообщения
    template<typename Actor, typename Method, typename MethodList>
    struct runtime_dispatch_helper;

    // Базовый случай - метод не найден
    template<typename Actor, typename Method>
    struct runtime_dispatch_helper<Actor, Method, type_traits::type_list<>> {
        template<typename ActorPtr, typename Sender, typename... Args>
        static bool dispatch(Method method, ActorPtr* actor, Sender sender, Args&&... args) {
            (void)method; (void)actor; (void)sender; (void)sizeof...(args);
            return false; // Метод не найден
        }
    };

    // Forward declaration для detail::dispatch_method_impl
    namespace detail {
        template<typename Actor, auto MethodPtr, uint64_t ActionId, typename ActorPtr, typename Sender, typename... Args>
        void dispatch_method_impl(ActorPtr* actor, Sender sender, Args&&... args);
    }

    // Рекурсивный случай - проверяем методы по очереди (с индексом для ActionId)
    template<typename Actor, typename Method, typename MethodList, uint64_t Index = 0>
    struct runtime_dispatch_with_index;

    // Базовый случай - метод не найден
    template<typename Actor, typename Method, uint64_t Index>
    struct runtime_dispatch_with_index<Actor, Method, type_traits::type_list<>, Index> {
        template<typename ActorPtr, typename Sender, typename... Args>
        static bool dispatch(Method method, ActorPtr* actor, Sender sender, Args&&... args) {
            (void)method; (void)actor; (void)sender; (void)sizeof...(args);
            return false;
        }
    };

    // Рекурсивный случай - проверяем первый метод
    template<typename Actor, typename Method, auto FirstMethodPtr, typename... Rest, uint64_t Index>
    struct runtime_dispatch_with_index<Actor, Method, type_traits::type_list<method_map_entry<FirstMethodPtr>, Rest...>, Index> {
        template<typename ActorPtr, typename Sender, typename... Args>
        static bool dispatch(Method method, ActorPtr* actor, Sender sender, Args&&... args) {
            // Сравниваем только если сигнатуры совпадают
            if constexpr (std::is_same<Method, decltype(FirstMethodPtr)>::value) {
                if (method == FirstMethodPtr) {
                    // Нашли! Отправляем сообщение с ActionId = Index
                    detail::dispatch_method_impl<Actor, FirstMethodPtr, Index>(
                        actor, sender, std::forward<Args>(args)...);
                    return true;
                }
            }
            // Ищем дальше с Index + 1
            return runtime_dispatch_with_index<Actor, Method, type_traits::type_list<Rest...>, Index + 1>::dispatch(
                method, actor, sender, std::forward<Args>(args)...);
        }
    };

    // Старый runtime_dispatch_helper теперь использует runtime_dispatch_with_index
    template<typename Actor, typename Method, auto FirstMethodPtr, typename... Rest>
    struct runtime_dispatch_helper<Actor, Method, type_traits::type_list<method_map_entry<FirstMethodPtr>, Rest...>> {
        template<typename ActorPtr, typename Sender, typename... Args>
        static bool dispatch(Method method, ActorPtr* actor, Sender sender, Args&&... args) {
            return runtime_dispatch_with_index<Actor, Method, type_traits::type_list<method_map_entry<FirstMethodPtr>, Rest...>, 0>::dispatch(
                method, actor, sender, std::forward<Args>(args)...);
        }
    };

} // namespace actor_zeta