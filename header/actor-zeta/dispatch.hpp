#pragma once

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/future.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/detail/rtt.hpp>

namespace actor_zeta {

namespace detail {

    /// @brief Copy result from method future to result_slot (if ready)
    /// @return true if result was copied (future was ready), false otherwise
    template<typename T>
    bool try_copy_result_to_slot(
        unique_future<T>& method_future,
        intrusive_ptr<detail::future_state_base> result_slot
    ) noexcept {
        if (!result_slot) {
            return false;  // No slot to copy to
        }

        if (!method_future.is_ready()) {
            return false;  // Not ready yet - user must poll
        }

        // Method future is ready - copy result to slot
        auto* method_state = method_future.get_state();
        if (!method_state) {
            return false;
        }

        // Take result from method state and set to slot
        result_slot->set_result_rtt(method_state->take_result());
        return true;
    }

    /// @brief Specialization for void futures
    inline bool try_copy_result_to_slot(
        unique_future<void>& method_future,
        intrusive_ptr<detail::future_state_base> result_slot
    ) noexcept {
        if (!result_slot) {
            return false;
        }

        if (!method_future.is_ready()) {
            return false;
        }

        // Void future is ready - mark slot as ready
        result_slot->set_state(detail::future_state_enum::ready);
        return true;
    }

} // namespace detail

    /// @brief Dispatch message to actor method
    /// @return unique_future with method result
    /// @note If method returns ready future, result is immediately copied to msg->result_slot()
    ///       If method returns pending future (suspended coroutine), user must poll and copy result
    template<class Actor, typename Method>
    auto dispatch(Actor* self, Method method, mailbox::message* msg) {
        using call_trait = type_traits::get_callable_trait_t<Method>;
        using result_type = typename call_trait::result_type;
        using args_type_list = typename call_trait::args_types;
        constexpr int args_size = call_trait::number_of_arguments;

        // Helper: try to copy result to slot if future is ready
        auto try_copy_to_slot = [msg](auto& future) {
            detail::try_copy_result_to_slot(future, msg->result_slot());
        };

        // Call method with unpacked arguments
        if constexpr (args_size == 0) {
            if constexpr (std::is_void_v<result_type>) {
                (self->*method)();
                auto future = make_ready_future_void(self->resource());
                try_copy_to_slot(future);
                return future;
            } else {
                auto future = (self->*method)();
                try_copy_to_slot(future);
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
                try_copy_to_slot(future);
                return future;
            } else {
                auto future = (self->*method)(std::forward<arg_type>(arg));
                try_copy_to_slot(future);
                return future;
            }
        } else {
            auto& args = msg->body();
            return [&]<std::size_t... I>(std::index_sequence<I...>) {
                if constexpr (std::is_void_v<result_type>) {
                    (self->*method)((detail::get<I, args_type_list>(args))...);
                    auto future = make_ready_future_void(self->resource());
                    try_copy_to_slot(future);
                    return future;
                } else {
                    auto future = (self->*method)((detail::get<I, args_type_list>(args))...);
                    try_copy_to_slot(future);
                    return future;
                }
            }(type_traits::make_index_sequence<args_size>{});
        }
    }

} // namespace actor_zeta