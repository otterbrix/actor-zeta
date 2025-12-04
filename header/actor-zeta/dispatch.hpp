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

    /// @brief Set up result chaining from method_future to msg->result_slot()
    template<typename T>
    void setup_result_chaining(
        unique_future<T>& method_future,
        mailbox::message* msg
    ) noexcept {
        auto result_slot_base = msg->result_slot();
        if (!result_slot_base) {
            return;
        }

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

    /// @brief Specialization for void futures
    inline void setup_result_chaining(
        unique_future<void>& method_future,
        mailbox::message* msg
    ) noexcept {
        auto result_slot_base = msg->result_slot();
        if (!result_slot_base) {
            return;
        }

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
        constexpr int args_size = call_trait::number_of_arguments;

        // =====================================================================
        // COMPILE-TIME VALIDATION: All argument types must be valid for RTT
        // =====================================================================
        static_assert(
            dispatch_validation::validate_args_list_v<args_type_list>,
            "dispatch(): method argument types must be valid for RTT storage "
            "(move/copy constructible, not abstract)"
        );

        // =====================================================================
        // COMPILE-TIME VALIDATION: No non-const lvalue references allowed
        // Non-const refs (T&) are semantically wrong: RTT stores copies,
        // changes to the copy don't affect the sender's original data.
        // Use: T (value), const T& (read-only ref), T&& (move)
        // =====================================================================
        static_assert(
            dispatch_validation::no_non_const_refs_v<args_type_list>,
            "dispatch(): non-const lvalue reference parameters (T&) are not allowed. "
            "Use value (T), const reference (const T&), or rvalue reference (T&&)"
        );

        // =====================================================================
        // RUNTIME VALIDATION: Message body size matches expected argument count
        // =====================================================================
        if constexpr (args_size > 0) {
            assert(msg->body().size() == static_cast<std::size_t>(args_size) &&
                   "dispatch(): message argument count mismatch");
        }

        auto setup_chaining = [msg](auto& future) {
            detail::setup_result_chaining(future, msg);
        };

        if constexpr (args_size == 0) {
            if constexpr (std::is_void_v<result_type>) {
                (self->*method)();
                auto future = make_ready_future_void(self->resource());
                setup_chaining(future);
                return future;
            } else {
                auto future = (self->*method)();
                setup_chaining(future);
                return future;
            }
        } else if constexpr (args_size == 1) {
            auto& args = msg->body();
            using arg_type = type_traits::type_list_at_t<args_type_list, 0>;
            using clear_arg_type = type_traits::decay_t<arg_type>;

            // Compile-time: validate single argument type
            static_assert(
                detail::is_valid_rtt_type_v<clear_arg_type>,
                "dispatch(): argument type must be valid for RTT storage"
            );

            // Use detail::get<> for consistency with multi-arg case
            // (std::move is inside detail::get<> in rtt.hpp)
            if constexpr (std::is_void_v<result_type>) {
                (self->*method)(detail::get<0, args_type_list>(args));
                auto future = make_ready_future_void(self->resource());
                setup_chaining(future);
                return future;
            } else {
                auto future = (self->*method)(detail::get<0, args_type_list>(args));
                setup_chaining(future);
                return future;
            }
        } else {
            auto& args = msg->body();
            return [&]<std::size_t... I>(std::index_sequence<I...>) {
                if constexpr (std::is_void_v<result_type>) {
                    (self->*method)((detail::get<I, args_type_list>(args))...);
                    auto future = make_ready_future_void(self->resource());
                    setup_chaining(future);
                    return future;
                } else {
                    auto future = (self->*method)((detail::get<I, args_type_list>(args))...);
                    setup_chaining(future);
                    return future;
                }
            }(type_traits::make_index_sequence<args_size>{});
        }
    }

} // namespace actor_zeta