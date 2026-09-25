#pragma once

#include <actor-zeta/actor/dispatch_traits.hpp>

namespace actor_zeta {

    namespace detail {

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

        template<>
        struct signatures_match<
            type_traits::type_list<>,
            type_traits::type_list<>
        > {
            static constexpr bool value = true;
        };

        template<typename Contract, auto... ActorMethods>
        concept methods_count_matches =
            type_traits::type_list_size_v<typename dispatch_traits_parser<ActorMethods...>::methods>
            == type_traits::type_list_size_v<typename Contract::dispatch_traits::methods>;

        template<auto... ActorMethods>
        concept all_methods_valid =
            dispatch_traits_parser<ActorMethods...>::all_valid;

        template<auto... ActorMethods>
        concept all_methods_no_const_ref =
            dispatch_traits_parser<ActorMethods...>::all_no_const_ref;

        template<auto... ActorMethods>
        concept all_methods_no_rvalue_ref =
            dispatch_traits_parser<ActorMethods...>::all_no_rvalue_ref;

        template<typename Contract, auto... ActorMethods>
        concept signatures_compatible =
            signatures_match<
                typename Contract::dispatch_traits::methods,
                typename dispatch_traits_parser<ActorMethods...>::methods
            >::value;

        template<typename Contract, auto... ActorMethods>
        concept valid_implementation =
            is_interface<Contract> &&
            methods_count_matches<Contract, ActorMethods...> &&
            all_methods_valid<ActorMethods...> &&
            all_methods_no_const_ref<ActorMethods...> &&
            all_methods_no_rvalue_ref<ActorMethods...> &&
            signatures_compatible<Contract, ActorMethods...>;

    } // namespace detail

    /// dispatch_traits bound to a contract: the actor's methods must match the contract's
    /// signatures one-to-one, and then msg_id<MyActor, &MyActor::m> == msg_id<my_contract, &my_contract::m>.
    ///
    ///   struct my_contract { unique_future<int> method1(int);
    ///                        using dispatch_traits = actor_zeta::dispatch_traits<&my_contract::method1>; };
    ///   class MyActor : ... { using dispatch_traits = actor_zeta::implements<my_contract, &MyActor::method1>; };
    template<typename Contract, auto... ActorMethods>
        requires detail::valid_implementation<Contract, ActorMethods...>
    struct implements {
    private:
        using parser = detail::dispatch_traits_parser<ActorMethods...>;

    public:
        using methods = typename parser::methods;

        using contract_type = Contract;
    };

} // namespace actor_zeta