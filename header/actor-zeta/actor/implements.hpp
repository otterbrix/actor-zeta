#pragma once

#include <actor-zeta/actor/dispatch_traits.hpp>

namespace actor_zeta {

    namespace detail {

        // Проверка совместимости сигнатур контракта и актора
        template<typename ContractMethods, typename ActorMethods>
        struct signatures_match;

        template<auto... ContractPtrs, auto... ActorPtrs>
        struct signatures_match<
            type_traits::type_list<method_map_entry<ContractPtrs>...>,
            type_traits::type_list<method_map_entry<ActorPtrs>...>
        > {
            template<auto C, auto A>
            static constexpr bool check_one() {
                using CT = type_traits::callable_trait<decltype(C)>;
                using AT = type_traits::callable_trait<decltype(A)>;
                return std::is_same_v<typename CT::result_type, typename AT::result_type>
                    && std::is_same_v<typename CT::args_types, typename AT::args_types>;
            }

            static constexpr bool value = (check_one<ContractPtrs, ActorPtrs>() && ...);
        };

        // Специализация для пустых списков
        template<>
        struct signatures_match<
            type_traits::type_list<>,
            type_traits::type_list<>
        > {
            static constexpr bool value = true;
        };

        // =====================================================================
        // C++20 Concepts для валидации implements<>
        // =====================================================================

        // Количество методов должно совпадать
        template<typename Contract, auto... ActorMethods>
        concept methods_count_matches =
            type_traits::type_list_size_v<typename dispatch_traits_parser<ActorMethods...>::methods>
            == type_traits::type_list_size_v<typename Contract::dispatch_traits::methods>;

        // Все методы актора должны возвращать unique_future<T> или generator<T>
        template<auto... ActorMethods>
        concept all_methods_valid =
            dispatch_traits_parser<ActorMethods...>::all_valid;

        // Корутины не должны иметь const& параметры
        template<auto... ActorMethods>
        concept all_methods_safe =
            dispatch_traits_parser<ActorMethods...>::all_safe;

        // Сигнатуры должны совпадать с контрактом
        template<typename Contract, auto... ActorMethods>
        concept signatures_compatible =
            signatures_match<
                typename Contract::dispatch_traits::methods,
                typename dispatch_traits_parser<ActorMethods...>::methods
            >::value;

        // Объединённый concept для всех проверок
        // Contract должен быть interface (has dispatch_traits, no mailbox)
        template<typename Contract, auto... ActorMethods>
        concept valid_implementation =
            is_interface<Contract> &&
            methods_count_matches<Contract, ActorMethods...> &&
            all_methods_valid<ActorMethods...> &&
            all_methods_safe<ActorMethods...> &&
            signatures_compatible<Contract, ActorMethods...>;

    } // namespace detail

    /// implements<> - расширение dispatch_traits с привязкой к контракту
    ///
    /// Использование:
    ///   struct my_contract {
    ///       unique_future<int> method1(int);
    ///       using dispatch_traits = actor_zeta::dispatch_traits<&my_contract::method1>;
    ///   };
    ///
    ///   class MyActor : public basic_actor<MyActor> {
    ///       unique_future<int> method1(int);
    ///       using dispatch_traits = actor_zeta::implements<
    ///           my_contract,
    ///           &MyActor::method1
    ///       >;
    ///   };
    ///
    /// msg_id<MyActor, &MyActor::method1> == msg_id<my_contract, &my_contract::method1>
    ///
    template<typename Contract, auto... ActorMethods>
        requires detail::valid_implementation<Contract, ActorMethods...>
    struct implements {
    private:
        using parser = detail::dispatch_traits_parser<ActorMethods...>;

    public:
        // === Совместимость с dispatch_traits ===
        using methods = typename parser::methods;

        // === Дополнительно для контракта ===
        using contract_type = Contract;
    };

} // namespace actor_zeta