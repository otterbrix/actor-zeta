#pragma once

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/future.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/detail/rtt.hpp>

namespace actor_zeta {

namespace detail {

    /// @brief Link future result to message result_slot via continuation (NON-BLOCKING)
    template<typename T>
    void link_future_to_slot(
        unique_future<T>& source,
        intrusive_ptr<detail::future_state_base> target_slot
    ) noexcept {
        // FAST PATH: Already ready - transfer result immediately
        if (source.is_ready()) {
            auto* source_state = source.get_state();
            if (!source_state) {
                return;
            }
            source_state->propagate_result_to(target_slot.get());
            return;
        }

        // SLOW PATH: NOT ready - set continuation (NO BLOCKING!)
        auto* source_state = source.get_state();
        source_state->set_continuation(target_slot);
    }

} // namespace detail

    /// @brief Dispatch message to actor method
    template<class Actor, typename Method>
    auto dispatch(Actor* self, Method method, mailbox::message* msg) {
        using call_trait = type_traits::get_callable_trait_t<Method>;
        using result_type = typename call_trait::result_type;
        using args_type_list = typename call_trait::args_types;
        constexpr int args_size = call_trait::number_of_arguments;

        // Helper lambda to link future to result_slot
        auto link_result = [msg](auto& future) {
            if (auto slot = msg->result_slot()) {
                detail::link_future_to_slot(future, slot);
            }
        };

        // Call method with unpacked arguments
        if constexpr (args_size == 0) {
            if constexpr (std::is_void_v<result_type>) {
                (self->*method)();
                return make_ready_future_void(self->resource());
            } else {
                auto future = (self->*method)();
                link_result(future);
                return future;
            }
        } else if constexpr (args_size == 1) {
            auto& args = msg->body();
            using arg_type = type_traits::type_list_at_t<args_type_list, 0>;
            using clear_arg_type = type_traits::decay_t<arg_type>;
            auto arg = args.get<clear_arg_type>(0);

            if constexpr (std::is_void_v<result_type>) {
                (self->*method)(std::forward<arg_type>(arg));
                return make_ready_future_void(self->resource());
            } else {
                auto future = (self->*method)(std::forward<arg_type>(arg));
                link_result(future);
                return future;
            }
        } else {
            auto& args = msg->body();
            return [&]<std::size_t... I>(std::index_sequence<I...>) {
                if constexpr (std::is_void_v<result_type>) {
                    (self->*method)((detail::get<I, args_type_list>(args))...);
                    return make_ready_future_void(self->resource());
                } else {
                    auto future = (self->*method)((detail::get<I, args_type_list>(args))...);
                    link_result(future);
                    return future;
                }
            }(type_traits::make_index_sequence<args_size>{});
        }
    }

} // namespace actor_zeta