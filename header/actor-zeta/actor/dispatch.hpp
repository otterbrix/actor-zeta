#pragma once

#include <actor-zeta/actor/forwards.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/future.hpp>
#include <actor-zeta/detail/generator.hpp>
#include <actor-zeta/detail/rtt.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/mailbox/make_message.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/mailbox/message_result.hpp>

namespace actor_zeta {

    namespace detail {

        // Chain method future result to message's result promise
        template<typename T>
        inline void setup_result_chaining_inline(
            unique_future<T>& method_future,
            mailbox::message* msg) noexcept {
            auto result_promise = msg->get_result_promise<T>();
            method_future.forward_to(result_promise);
        }

        // Link method's generator to message's external generator state
        template<typename T>
        inline void setup_generator_linking_inline(
            generator<T>& method_generator,
            mailbox::message* msg) noexcept {
            auto* external_state = msg->template get_generator_state<T>();
            if (external_state && method_generator.internal_state()) {
                method_generator.link_to(external_state);

                // Start the coroutine
                auto* internal_state = method_generator.internal_state();
                auto producer = internal_state->take_producer_handle();
                if (producer && !producer.done()) {
                    producer.resume();
                }
            }
        }

    } // namespace detail

    namespace dispatch_validation {

        // T is non-const lvalue reference (T& but not const T&)
        template<typename T>
        concept non_const_lvalue_ref = std::is_lvalue_reference_v<T> && !std::is_const_v<std::remove_reference_t<T>>;

        // Any type in parameter pack is non-const lvalue ref
        template<typename... Args>
        concept has_any_non_const_lvalue_ref = (non_const_lvalue_ref<Args> || ...);

        // All types in parameter pack are valid for RTT storage
        template<typename... Args>
        concept all_valid_rtt_types = (detail::is_valid_rtt_type_v<type_traits::decay_t<Args>> && ...);

        namespace detail {
            template<typename ArgsList>
            struct args_list_traits;

            template<typename... Args>
            struct args_list_traits<type_traits::type_list<Args...>> {
                static constexpr bool all_valid = all_valid_rtt_types<Args...>;
                static constexpr bool no_non_const_refs = !has_any_non_const_lvalue_ref<Args...>;
            };

            template<>
            struct args_list_traits<type_traits::type_list<>> {
                static constexpr bool all_valid = true;
                static constexpr bool no_non_const_refs = true;
            };
        }

        // All argument types in type_list are valid for RTT storage
        template<typename ArgsList>
        concept valid_args_list = detail::args_list_traits<ArgsList>::all_valid;

        // No non-const lvalue refs in type_list
        template<typename ArgsList>
        concept no_non_const_refs_in_list = detail::args_list_traits<ArgsList>::no_non_const_refs;

        template<typename ArgsList>
        inline constexpr bool validate_args_list_v = valid_args_list<ArgsList>;

        template<typename ArgsList>
        inline constexpr bool no_non_const_refs_v = no_non_const_refs_in_list<ArgsList>;

    } // namespace dispatch_validation

    // Dispatch message to actor method, returns unique_future<T> or generator<T>
    template<class Actor, typename Method>
    auto dispatch(Actor* self, Method method, mailbox::message* msg) {
        using call_trait = type_traits::get_callable_trait_t<Method>;
        using result_type = typename call_trait::result_type;
        using args_type_list = typename call_trait::args_types;
        constexpr std::size_t args_size = call_trait::number_of_arguments;

        static_assert(
            dispatch_validation::validate_args_list_v<args_type_list>,
            "dispatch(): method argument types must be valid for RTT storage "
            "(move/copy constructible, not abstract)");

        static_assert(
            dispatch_validation::no_non_const_refs_v<args_type_list>,
            "dispatch(): non-const lvalue reference parameters (T&) are not allowed. "
            "Use value (T), const reference (const T&), or rvalue reference (T&&)");

        if constexpr (args_size > 0) {
            assert(msg->body().size() == args_size &&
                   "dispatch(): message argument count mismatch");
        }

        auto invoke_method = [&]<std::size_t... I>(std::index_sequence<I...>) {
            if constexpr (args_size == 0) {
                return (self->*method)();
            } else {
                auto& args = msg->body();
                return (self->*method)((detail::get<I, args_type_list>(args))...);
            }
        };


        static_assert(
            type_traits::is_unique_future_v<result_type> || type_traits::is_generator_v<result_type>,
            "dispatch(): Actor methods must return unique_future<T> or generator<T>. "
            "Raw void or value returns are not allowed. "
            "All actor methods must be coroutines.");

        if constexpr (type_traits::is_generator_v<result_type>) {
            auto gen = invoke_method(std::make_index_sequence<args_size>{});
            detail::setup_generator_linking_inline(gen, msg);
            return gen;
        } else {
            auto future = invoke_method(std::make_index_sequence<args_size>{});
            detail::setup_result_chaining_inline(future, msg);
            return future;
        }
    }

} // namespace actor_zeta