#pragma once

#include <actor-zeta/actor/dispatch_traits.hpp>
#include <actor-zeta/mailbox/make_message.hpp>

namespace actor_zeta {

    namespace detail {

        // Compile-time argument validation

        template<typename Actor, auto MethodPtr, typename... Args>
        struct validate_send_args {
            using callable_trait = type_traits::callable_trait<decltype(MethodPtr)>;
            using method_result_type = typename callable_trait::result_type;
            using expected_args = typename callable_trait::args_types;
            using provided_args = type_traits::type_list<Args...>;

            static constexpr size_t expected_count = callable_trait::number_of_arguments;
            static constexpr size_t provided_count = sizeof...(Args);

            static_assert(
                expected_count == provided_count,
                "send(): argument count mismatch - check method signature");

            static_assert(
                args_compatible_v<expected_args, provided_args>,
                "send(): argument types are not compatible with method signature");

            static_assert(
                all_args_storable_v<Args...>,
                "send(): all arguments must be storable in message "
                "(move/copy constructible, not abstract)");
        };

        // Dispatch implementation - creates message and calls enqueue_impl

        template<typename Actor, auto MethodPtr, uint64_t ActionId, typename ActorPtr, typename... Args>
        inline auto dispatch_method_impl(ActorPtr* actor, Args&&... args)
            -> send_result_t<Actor, typename type_traits::callable_trait<decltype(MethodPtr)>::result_type> {
            (void)validate_send_args<Actor, MethodPtr, Args...>{};

            using callable_trait = type_traits::callable_trait<decltype(MethodPtr)>;
            using method_result_type = typename callable_trait::result_type;

            auto cmd = mailbox::make_message_id(ActionId);

            if constexpr (type_traits::is_unique_future_v<method_result_type>) {
                using value_type = typename type_traits::is_unique_future<method_result_type>::value_type;

                // Create message with shared_state for the result
                auto [msg, future] = detail::make_message<value_type>(
                    actor->resource(), cmd, std::forward<Args>(args)...);

                // Enqueue the message
                auto [needs_sched, result] = actor->enqueue_impl(std::move(msg));

                // On queue_closed, the message destructor will automatically call cleanup_fn_
                // which sets error and releases promise - no need to manually handle it
                ignore_unused(result);

                // Return pair<bool, future> for new API
                return {needs_sched, std::move(future)};

            } else if constexpr (type_traits::is_generator_v<method_result_type>) {
                using value_type = typename method_result_type::value_type;

                // Create message with generator_state
                auto [msg, gen] = detail::make_generator_message<value_type>(
                    actor->resource(), cmd, std::forward<Args>(args)...);

                auto [needs_sched, enq_result] = actor->enqueue_impl(std::move(msg));

                if (enq_result == enqueue_result::queue_closed) {
                    gen.cancel();
                }

                return {needs_sched, std::move(gen)};
            }
        }

        // Dispatch for address_t (interface polymorphism)

        template<typename Interface, auto MethodPtr, uint64_t ActionId, typename... Args>
        inline auto dispatch_method_impl_address(actor::address_t target, Args&&... args)
            -> send_result_t<Interface, typename type_traits::callable_trait<decltype(MethodPtr)>::result_type> {
            (void)validate_send_args<Interface, MethodPtr, Args...>{};

            using callable_trait = type_traits::callable_trait<decltype(MethodPtr)>;
            using method_result_type = typename callable_trait::result_type;

            auto cmd = mailbox::make_message_id(ActionId);

            if constexpr (type_traits::is_unique_future_v<method_result_type>) {
                using value_type = typename type_traits::is_unique_future<method_result_type>::value_type;

                // Create message with shared_state for the result
                auto [msg, future] = detail::make_message<value_type>(
                    target.resource(), cmd, std::forward<Args>(args)...);

                // Enqueue the message
                auto [needs_sched, result] = target.enqueue_impl(std::move(msg));

                // On queue_closed, the message destructor will automatically call cleanup_fn_
                ignore_unused(result);

                // Return pair<bool, future> for new API
                return {needs_sched, std::move(future)};

            } else if constexpr (type_traits::is_generator_v<method_result_type>) {
                using value_type = typename method_result_type::value_type;

                // Create message with generator_state
                auto [msg, gen] = detail::make_generator_message<value_type>(
                    target.resource(), cmd, std::forward<Args>(args)...);

                auto [needs_sched, enq_result] = target.enqueue_impl(std::move(msg));

                if (enq_result == enqueue_result::queue_closed) {
                    gen.cancel();
                }

                return {needs_sched, std::move(gen)};
            }
        }

    } // namespace detail

    // send() for Actor* (direct actor pointer)

    template<typename ActorPtr, typename Method, typename... Args,
             typename Actor = typename type_traits::callable_trait<Method>::class_type>
    [[nodiscard]] inline auto send(ActorPtr* actor, Method method, Args&&... args)
        -> detail::send_result_t<Actor, typename type_traits::callable_trait<Method>::result_type> {
        using result_type = typename type_traits::callable_trait<Method>::result_type;

        static_assert(!std::is_same_v<result_type, bool>,
                      "Actor methods must not return bool. "
                      "Use void or other types - all return results via promise/future.");

        using methods = typename Actor::dispatch_traits::methods;

        return runtime_dispatch_helper<Actor, Method, methods>::dispatch(
            method, actor, std::forward<Args>(args)...);
    }

    // send() for address_t (interface polymorphism)

    template<typename Method, typename... Args,
             typename Interface = typename type_traits::callable_trait<Method>::class_type>
        requires detail::is_interface<Interface>
    [[nodiscard]] inline auto send(actor::address_t target, Method method, Args&&... args)
        -> detail::send_result_t<Interface, typename type_traits::callable_trait<Method>::result_type> {
        using result_type = typename type_traits::callable_trait<Method>::result_type;

        static_assert(!std::is_same_v<result_type, bool>,
                      "Actor methods must not return bool. "
                      "Use void or other types - all return results via promise/future.");

        assert(target && "target address must not be empty");

        using methods = typename Interface::dispatch_traits::methods;

        return runtime_dispatch_helper_address<Interface, Method, methods>::dispatch(
            method, target, std::forward<Args>(args)...);
    }

} // namespace actor_zeta
