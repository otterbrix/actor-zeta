#pragma once

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/future.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/detail/rtt.hpp>

namespace actor_zeta {

namespace detail {

    /// @brief Set up result chaining from method_future to msg->result_slot()
    /// When method_future becomes ready, result is automatically forwarded
    template<typename T>
    void setup_result_chaining(
        unique_future<T>& method_future,
        mailbox::message* msg
    ) noexcept {
        auto* method_state = method_future.get_state();
        if (!method_state) {
            return;
        }

        auto result_slot = msg->result_slot();
        if (!result_slot) {
            return;
        }

        // Set up chaining: method_state -> result_slot
        // When method_state becomes ready, it will automatically forward to result_slot
        method_state->set_forward_target(result_slot.get());

        // If method already completed (sync method like make_ready_future),
        // result was set BEFORE chaining was established. Forward manually now.
        if (method_state->is_ready()) {
            // Move result to forward target (method_state becomes consumed)
            result_slot->set_result_rtt(method_state->take_result());
        }
    }

    /// @brief Specialization for void futures
    inline void setup_result_chaining(
        unique_future<void>& method_future,
        mailbox::message* msg
    ) noexcept {
        auto* method_state = method_future.get_state();
        if (!method_state) {
            return;
        }

        auto result_slot = msg->result_slot();
        if (!result_slot) {
            return;
        }

        // Set up chaining for void
        method_state->set_forward_target(result_slot.get());

        // If already ready (sync void method), mark slot as ready
        if (method_state->is_ready()) {
            result_slot->set_state(future_state_enum::ready);
        }
    }

} // namespace detail

    /// @brief Dispatch message to actor method
    /// @return unique_future with method result
    /// @note Result is automatically forwarded to msg->result_slot() via chaining
    ///       - If method returns ready future, forward happens immediately in set_result_rtt()
    ///       - If method returns pending future (coroutine), forward happens when coroutine completes
    template<class Actor, typename Method>
    auto dispatch(Actor* self, Method method, mailbox::message* msg) {
        using call_trait = type_traits::get_callable_trait_t<Method>;
        using result_type = typename call_trait::result_type;
        using args_type_list = typename call_trait::args_types;
        constexpr int args_size = call_trait::number_of_arguments;

        // Helper: set up chaining for automatic result propagation
        auto setup_chaining = [msg](auto& future) {
            detail::setup_result_chaining(future, msg);
        };

        // Call method with unpacked arguments
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
            auto arg = args.get<clear_arg_type>(0);

            if constexpr (std::is_void_v<result_type>) {
                (self->*method)(std::forward<arg_type>(arg));
                auto future = make_ready_future_void(self->resource());
                setup_chaining(future);
                return future;
            } else {
                auto future = (self->*method)(std::forward<arg_type>(arg));
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