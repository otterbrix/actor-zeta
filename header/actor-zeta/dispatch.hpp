#pragma once

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/future.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/detail/rtt.hpp>
#include <actor-zeta/make_message.hpp>  // for is_valid_rtt_type_v

namespace actor_zeta {

namespace detail {

    /// @brief Set up result chaining from method_future to msg->result_slot() (inline version)
    /// @note Unified for both void and non-void types using if constexpr
    template<typename T>
    inline void setup_result_chaining_inline(
        unique_future<T>& method_future,
        mailbox::message* msg
    ) noexcept {
        auto result_slot_base = msg->result_slot();
        if (!result_slot_base) {
            return;
        }

        if constexpr (std::is_void_v<T>) {
            auto* result_slot = static_cast<detail::future_state<void>*>(result_slot_base.get());

            if (method_future.available()) {
                result_slot->set_ready();
                return;
            }

            auto* method_state = method_future.get_state();
            if (!method_state) {
                assert(false && "Non-available void future has no state!");
                result_slot->set_state(detail::future_state_enum::error);
                return;
            }

            method_state->set_forward_target_void(result_slot);
        } else {
            auto* result_slot = static_cast<detail::future_state<T>*>(result_slot_base.get());

            if (method_future.available()) {
                result_slot->set_value(std::move(method_future).get());
                return;
            }

            auto* method_state = method_future.get_state();
            if (!method_state) {
                assert(false && "Non-available typed future has no state!");
                result_slot->set_state(detail::future_state_enum::error);
                return;
            }

            method_state->set_forward_target(result_slot);
        }
    }

} // namespace detail

    // =========================================================================
    // Compile-time validation helpers for dispatch()
    // =========================================================================

    namespace dispatch_validation {

        /// @brief Check if type is non-const lvalue reference (T& but not const T&)
        template<typename T>
        struct is_non_const_lvalue_ref : std::false_type {};

        template<typename T>
        struct is_non_const_lvalue_ref<T&> : std::bool_constant<!std::is_const_v<T>> {};

        template<typename T>
        inline constexpr bool is_non_const_lvalue_ref_v = is_non_const_lvalue_ref<T>::value;

        /// @brief Validate that all argument types in the type_list are valid for RTT storage
        /// and do NOT contain non-const lvalue references (T&)
        template<typename ArgsList>
        struct validate_args_list;

        template<>
        struct validate_args_list<type_traits::type_list<>> {
            static constexpr bool value = true;
            static constexpr bool no_non_const_refs = true;
        };

        template<typename Head, typename... Tail>
        struct validate_args_list<type_traits::type_list<Head, Tail...>> {
            using decay_head = type_traits::decay_t<Head>;
            static constexpr bool value =
                detail::is_valid_rtt_type_v<decay_head> &&
                validate_args_list<type_traits::type_list<Tail...>>::value;
            static constexpr bool no_non_const_refs =
                !is_non_const_lvalue_ref_v<Head> &&
                validate_args_list<type_traits::type_list<Tail...>>::no_non_const_refs;
        };

        template<typename ArgsList>
        inline constexpr bool validate_args_list_v = validate_args_list<ArgsList>::value;

        template<typename ArgsList>
        inline constexpr bool no_non_const_refs_v = validate_args_list<ArgsList>::no_non_const_refs;

    } // namespace dispatch_validation

    /// @brief Dispatch message to actor method, returns unique_future with result
    ///
    /// Optimized version:
    /// - Unified code path for all argument counts using index_sequence
    /// - No lambda allocation for setup_chaining
    /// - Single if constexpr for void/non-void handling
    ///
    /// Compile-time validation:
    /// - All argument types must be valid for RTT storage
    ///
    /// Runtime validation:
    /// - Message body size matches expected argument count
    template<class Actor, typename Method>
    auto dispatch(Actor* self, Method method, mailbox::message* msg) {
        using call_trait = type_traits::get_callable_trait_t<Method>;
        using result_type = typename call_trait::result_type;
        using args_type_list = typename call_trait::args_types;
        constexpr std::size_t args_size = call_trait::number_of_arguments;

        // =====================================================================
        // COMPILE-TIME VALIDATION
        // =====================================================================
        static_assert(
            dispatch_validation::validate_args_list_v<args_type_list>,
            "dispatch(): method argument types must be valid for RTT storage "
            "(move/copy constructible, not abstract)"
        );

        static_assert(
            dispatch_validation::no_non_const_refs_v<args_type_list>,
            "dispatch(): non-const lvalue reference parameters (T&) are not allowed. "
            "Use value (T), const reference (const T&), or rvalue reference (T&&)"
        );

        // =====================================================================
        // RUNTIME VALIDATION
        // =====================================================================
        if constexpr (args_size > 0) {
            assert(msg->body().size() == args_size &&
                   "dispatch(): message argument count mismatch");
        }

        // =====================================================================
        // UNIFIED DISPATCH - single code path for all cases
        // =====================================================================
        auto invoke_method = [&]<std::size_t... I>(std::index_sequence<I...>) {
            if constexpr (args_size == 0) {
                return (self->*method)();
            } else {
                auto& args = msg->body();
                return (self->*method)((detail::get<I, args_type_list>(args))...);
            }
        };

        auto future = invoke_method(std::make_index_sequence<args_size>{});

        // Inline result chaining (no lambda overhead)
        detail::setup_result_chaining_inline(future, msg);

        return future;
    }

} // namespace actor_zeta