#pragma once

#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/future.hpp>
#include <actor-zeta/detail/shared_state.hpp>
#include <actor-zeta/mailbox/message.hpp>

namespace actor_zeta {

    namespace dispatch_validation {

        template<typename T>
        concept non_const_lvalue_ref = std::is_lvalue_reference_v<T> && !std::is_const_v<std::remove_reference_t<T>>;

        template<typename... Args>
        concept has_any_non_const_lvalue_ref = (non_const_lvalue_ref<Args> || ...);

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

        template<typename ArgsList>
        concept valid_args_list = detail::args_list_traits<ArgsList>::all_valid;

        template<typename ArgsList>
        concept no_non_const_refs_in_list = detail::args_list_traits<ArgsList>::no_non_const_refs;

        template<typename ArgsList>
        inline constexpr bool validate_args_list_v = valid_args_list<ArgsList>;

        template<typename ArgsList>
        inline constexpr bool no_non_const_refs_v = no_non_const_refs_in_list<ArgsList>;

    } // namespace dispatch_validation

    // Extract args from the message body and call the method (NOT a coroutine).
    template<class Actor, typename Method, typename ArgsTypeList, std::size_t ArgsSize>
    auto invoke_actor_method(Actor* self, Method method, mailbox::message* msg) {
        if constexpr (ArgsSize == 0) {
            return (self->*method)();
        } else {
            return [&]<std::size_t... I>(std::index_sequence<I...>) {
                auto& args = msg->body();
                return (self->*method)((detail::get<I, ArgsTypeList>(args))...);
            }(std::make_index_sequence<ArgsSize>{});
        }
    }

    // Dispatch a message to an actor method; method's result is forwarded to the
    // caller's promise via the message's result_slot.
    template<class Actor, typename Method>
    unique_future<void> dispatch(Actor* self, Method method, mailbox::message* msg) {
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

        static_assert(
            type_traits::is_unique_future_v<result_type>,
            "dispatch(): Actor methods must return unique_future<T>. "
            "Raw void or value returns are not allowed. "
            "All actor methods must be coroutines using co_return.");

        if constexpr (args_size > 0) {
            assert(msg->body().size() == args_size &&
                   "dispatch(): message argument count mismatch");
        }

        // co_await the method, set_value on the caller's promise.
        //
        // ORDER IS LOAD-BEARING: get_result_promise() reads result_slot_, and both
        // it and transfer_ownership() must run BEFORE the first suspension below --
        // past that co_await, resume_impl's message_guard destroys the message.
        using value_type = typename type_traits::is_unique_future<result_type>::value_type;

        auto result_promise = msg->template get_result_promise<value_type>();
        msg->transfer_ownership();   // ~message won't run cleanup anymore
        auto method_future = invoke_actor_method<Actor, Method, args_type_list, args_size>(self, method, msg);

        if constexpr (std::is_void_v<value_type>) {
            co_await std::move(method_future);
            result_promise.set_value();
        } else {
            auto value = co_await std::move(method_future);
            result_promise.set_value(std::move(value));
        }
        co_return;
    }

} // namespace actor_zeta
