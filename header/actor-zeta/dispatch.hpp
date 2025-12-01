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
    /// Uses TYPED forwarding - no RTT needed!
    /// @note Handles both state-based futures AND inline storage futures (Fast Ready Future Factory)
    template<typename T>
    void setup_result_chaining(
        unique_future<T>& method_future,
        mailbox::message* msg
    ) noexcept {
        auto result_slot_base = msg->result_slot();
        if (!result_slot_base) {
            return;
        }

        // Safe cast: result_slot was created by send<T>() which creates future_state<T>
        auto* result_slot = static_cast<detail::future_state<T>*>(result_slot_base.get());

        // Check if method_future is already available (handles both cases):
        // 1. Inline storage (make_ready_future with Fast Ready Future Factory) - has_inline_value_ is true
        // 2. State-based ready future - state_->is_ready() is true
        if (method_future.available()) {
            // Method already completed - forward result immediately
            // This handles BOTH inline storage AND state-based ready futures
            // TYPED forwarding - ZERO ALLOCATION!
            result_slot->set_value(std::move(method_future).get());
            return;
        }

        // Method not ready - must have valid state for async chaining
        auto* method_state = method_future.get_state();
        if (!method_state) {
            // CRITICAL: Future claims not-available but has no state!
            // This indicates a bug in make_ready_future() or unique_future.
            // Without state, we cannot set up chaining → caller's future will hang forever.
            assert(false && "Non-available typed future has no state - bug in future implementation!");
            // In release: mark result_slot as error to prevent infinite hang
            result_slot->set_state(detail::future_state_enum::error);
            return;
        }

        // Set up chaining for async: method_state -> result_slot (TYPED!)
        method_state->set_forward_target(result_slot);
    }

    /// @brief Specialization for void futures
    /// @note Handles both stateful futures AND zero-allocation ready void futures (is_ready_void_)
    inline void setup_result_chaining(
        unique_future<void>& method_future,
        mailbox::message* msg
    ) noexcept {
        auto result_slot_base = msg->result_slot();
        if (!result_slot_base) {
            return;
        }

        // Safe cast: result_slot was created by send<void>() which creates future_state<void>
        auto* result_slot = static_cast<detail::future_state<void>*>(result_slot_base.get());

        // Check if method_future is already available (handles both cases):
        // 1. Zero-allocation ready void (make_ready_future_void) - state_ is nullptr, is_ready_void_ is true
        // 2. Stateful ready future - state_ is valid, state_->is_ready() is true
        if (method_future.available()) {
            // Method already completed - set result_slot ready immediately
            result_slot->set_ready();
            return;
        }

        // Method not ready - must have valid state for chaining
        auto* method_state = method_future.get_state();
        if (!method_state) {
            // CRITICAL: Future claims not-available but has no state!
            // This indicates a bug in make_ready_future_void() or unique_future.
            // Without state, we cannot set up chaining → caller's future will hang forever.
            assert(false && "Non-available void future has no state - bug in future implementation!");
            // In release: mark result_slot as error to prevent infinite hang
            result_slot->set_state(detail::future_state_enum::error);
            return;
        }

        // Set up chaining for async void (via virtual method)
        method_state->set_forward_target_void(result_slot);
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