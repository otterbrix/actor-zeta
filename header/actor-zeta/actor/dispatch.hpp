#pragma once

#include <actor-zeta/actor/forwards.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/future.hpp>
#include <actor-zeta/detail/rtt.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/mailbox/make_message.hpp>
#include <actor-zeta/mailbox/message.hpp>

namespace actor_zeta {

    namespace detail {

        /// @brief Set up result chaining from method_future to msg's result promise
        /// @note Uses forward_to(promise&) - clean API without exposing detail::
        template<typename T>
        inline void setup_result_chaining_inline(
            unique_future<T>& method_future,
            mailbox::message* msg) noexcept {
            auto result_promise = msg->get_result_promise<T>();
            method_future.forward_to(result_promise);
        }

    } // namespace detail

    // =========================================================================
    // Compile-time validation helpers for dispatch()
    // =========================================================================

    namespace dispatch_validation {

        /// @brief Concept: T is non-const lvalue reference (T& but not const T&)
        template<typename T>
        concept non_const_lvalue_ref = std::is_lvalue_reference_v<T> && !std::is_const_v<std::remove_reference_t<T>>;

        /// @brief Concept: Any type in parameter pack is non-const lvalue ref
        template<typename... Args>
        concept has_any_non_const_lvalue_ref = (non_const_lvalue_ref<Args> || ...);

        /// @brief Concept: All types in parameter pack are valid for RTT storage
        template<typename... Args>
        concept all_valid_rtt_types = (detail::is_valid_rtt_type_v<type_traits::decay_t<Args>> && ...);

        /// @brief Helper to extract types from type_list for concept
        namespace detail {
            template<typename ArgsList>
            struct args_list_traits;

            template<typename... Args>
            struct args_list_traits<type_traits::type_list<Args...>> {
                static constexpr bool all_valid = all_valid_rtt_types<Args...>;
                static constexpr bool no_non_const_refs = !has_any_non_const_lvalue_ref<Args...>;
            };

            // Empty list specialization
            template<>
            struct args_list_traits<type_traits::type_list<>> {
                static constexpr bool all_valid = true;
                static constexpr bool no_non_const_refs = true;
            };
        }

        /// @brief Concept: all argument types in type_list are valid for RTT storage
        template<typename ArgsList>
        concept valid_args_list = detail::args_list_traits<ArgsList>::all_valid;

        /// @brief Concept: no non-const lvalue refs in type_list
        template<typename ArgsList>
        concept no_non_const_refs_in_list = detail::args_list_traits<ArgsList>::no_non_const_refs;

        // Legacy compatibility
        template<typename ArgsList>
        inline constexpr bool validate_args_list_v = valid_args_list<ArgsList>;

        template<typename ArgsList>
        inline constexpr bool no_non_const_refs_v = no_non_const_refs_in_list<ArgsList>;

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
            "(move/copy constructible, not abstract)");

        static_assert(
            dispatch_validation::no_non_const_refs_v<args_type_list>,
            "dispatch(): non-const lvalue reference parameters (T&) are not allowed. "
            "Use value (T), const reference (const T&), or rvalue reference (T&&)");

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

        // =====================================================================
        // COROUTINE-ONLY: All methods must return unique_future<T>
        // =====================================================================
        static_assert(
            type_traits::is_unique_future_v<result_type>,
            "dispatch(): Actor methods must return unique_future<T>. "
            "Raw void or value returns are not allowed. "
            "All actor methods must be coroutines using co_return.");

        // Method returns unique_future<T> - use result chaining
        auto future = invoke_method(std::make_index_sequence<args_size>{});
        detail::setup_result_chaining_inline(future, msg);
        return future;
    }

} // namespace actor_zeta