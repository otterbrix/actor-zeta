#pragma once

#include <cstdint>
#include <actor-zeta/detail/type_list.hpp>
#include <actor-zeta/mailbox/id.hpp>
#include <actor-zeta/base/forwards.hpp>

namespace actor_zeta {

    /// @brief Compile-time map entry: (MethodPtr -> ActionId)
    /// ActionId генерируется автоматически на основе позиции в списке
    template<auto MethodPtr>
    struct method_map_entry {};

    /// @brief Короткий алиас для method_map_entry
    template<auto MethodPtr>
    using method = method_map_entry<MethodPtr>;

    /// @brief Шаблон dispatch_traits для удобного синтаксиса внутри актора
    /// Позволяет писать: using dispatch_traits = dispatch_traits<&MyActor::method1, &MyActor::method2>;
    template<auto... MethodPtrs>
    struct dispatch_traits {
        using methods = type_traits::type_list<method_map_entry<MethodPtrs>...>;
    };

    /// @brief Compile-time поиск action_id по методу через variadic expansion
    namespace detail {
        // Хелпер для сравнения одного метода (работает только если типы совпадают)
        template<auto SearchPtr, auto CurrentPtr>
        struct is_same_method_ptr : std::false_type {};

        // Специализация для одинаковых типов - можем сравнивать значения
        template<auto Ptr>
        struct is_same_method_ptr<Ptr, Ptr> : std::true_type {};

        // Compile-time поиск через constexpr цикл
        template<auto SearchPtr, auto... MethodPtrs>
        static constexpr uint64_t find_method_index() {
            // Создаем массив совпадений через pack expansion
            constexpr bool matches[] = {is_same_method_ptr<SearchPtr, MethodPtrs>::value...};

            // Линейный поиск первого совпадения
            for (std::size_t i = 0; i < sizeof...(MethodPtrs); ++i) {
                if (matches[i]) {
                    return static_cast<uint64_t>(i);
                }
            }
            return 0; // Не найден
        }
    }

    /// @brief Получение action_id для метода из Actor::dispatch_traits (compile-time)
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
    /// case msg_id<MyActor, &MyActor::insert>:
    /// @endcode
    template<typename Actor, auto MethodPtr, typename MethodList>
    struct action_id_impl;

    // Специализация для type_list с извлечением всех MethodPtrs
    template<typename Actor, auto SearchPtr, auto... MethodPtrs>
    struct action_id_impl<Actor, SearchPtr, type_traits::type_list<method_map_entry<MethodPtrs>...>> {
        static constexpr uint64_t value = detail::find_method_index<SearchPtr, MethodPtrs...>();
    };

    /// @brief Compile-time constexpr переменная для message_id
    /// Объединяет make_message_id + action_id для максимально компактного синтаксиса
    template<typename Actor, auto MethodPtr>
    inline constexpr auto msg_id = mailbox::make_message_id(
        action_id_impl<Actor, MethodPtr, typename Actor::dispatch_traits::methods>::value
    );

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
        bool dispatch_method_impl(ActorPtr* actor, Sender sender, Args&&... args);
    }

    // Нерекурсивный хелпер для dispatch одного метода
    // Returns: true if method found AND actor was unblocked (needs scheduling)
    template<typename Actor, typename Method, auto MethodPtr, uint64_t Index, typename ActorPtr, typename Sender, typename... Args>
    static bool try_dispatch_one(Method method, ActorPtr* actor, Sender sender, Args&&... args) {
        // Сравниваем только если сигнатуры совпадают
        if constexpr (std::is_same<Method, decltype(MethodPtr)>::value) {
            if (method == MethodPtr) {
                // Нашли! Отправляем сообщение с ActionId = Index
                // Возвращаем результат enqueue() - true если актор разблокирован
                return detail::dispatch_method_impl<Actor, MethodPtr, Index>(
                    actor, sender, std::forward<Args>(args)...);
            }
        }
        return false;
    }

    // Нерекурсивная реализация через fold expression
    template<typename Actor, typename Method, auto... MethodPtrs, typename ActorPtr, typename Sender, typename... Args, std::size_t... Is>
    static bool dispatch_impl(Method method, ActorPtr* actor, Sender sender, std::index_sequence<Is...>, Args&&... args) {
        // Используем fold expression с оператором || для short-circuit evaluation
        return (try_dispatch_one<Actor, Method, MethodPtrs, Is>(method, actor, sender, std::forward<Args>(args)...) || ...);
    }

    // Специализация runtime_dispatch_helper с извлечением MethodPtrs из type_list
    template<typename Actor, typename Method, auto... MethodPtrs>
    struct runtime_dispatch_helper<Actor, Method, type_traits::type_list<method_map_entry<MethodPtrs>...>> {
        // Перегрузка для ActorPtr*
        template<typename ActorPtr, typename Sender, typename... Args>
        static bool dispatch(Method method, ActorPtr* actor, Sender sender, Args&&... args) {
            return dispatch_impl<Actor, Method, MethodPtrs...>(
                method, actor, sender,
                std::index_sequence_for<method_map_entry<MethodPtrs>...>{},
                std::forward<Args>(args)...);
        }

        // Перегрузка для address_t
        template<typename Sender, typename... Args>
        static bool dispatch(Method method, base::address_t target, Sender sender, Args&&... args) {
            auto* actor = static_cast<Actor*>(target.operator->());
            return dispatch_impl<Actor, Method, MethodPtrs...>(
                method, actor, sender,
                std::index_sequence_for<method_map_entry<MethodPtrs>...>{},
                std::forward<Args>(args)...);
        }
    };

} // namespace actor_zeta