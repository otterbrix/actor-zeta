#pragma once

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/future.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/impl/handler.ipp>

namespace actor_zeta {

    /// @brief Dispatch message to actor method (NEW API - replaces behavior_t)
    /// @tparam Actor Actor type (deduced from method pointer)
    /// @tparam Method Method pointer type
    /// @param self Actor instance pointer
    /// @param method Method pointer (e.g., &Actor::compute)
    /// @param msg Message with arguments in body()
    /// @return unique_future<void> for behavior() chaining
    ///
    /// @details
    /// dispatch() does:
    /// 1. Unpack arguments from msg->body() using callable_trait
    /// 2. Call (self->*method)(args...)
    /// 3. Link result future to msg->result_slot() via continuation (non-blocking!)
    /// 4. Return future<void> for behavior() chaining
    ///
    /// @note This is the NEW recommended API - replaces behavior_t mechanism
    /// @note Library NEVER calls .get() - uses continuation for non-blocking async
    ///
    /// Usage inside actor's behavior():
    /// @code
    /// unique_future<void> behavior(message* msg) {
    ///     switch (msg->command()) {
    ///         case msg_id<Actor, &Actor::compute>:
    ///             return dispatch(this, &Actor::compute, msg);  // NEW API!
    ///     }
    ///     return make_ready_future<void>(resource());
    /// }
    /// @endcode
    template<class Actor, typename Method>
    auto dispatch(Actor* self, Method method, mailbox::message* msg) -> unique_future<void> {
        using call_trait = type_traits::get_callable_trait_t<Method>;
        using result_type = typename call_trait::result_type;
        using args_type_list = typename call_trait::args_types;
        constexpr int args_size = call_trait::number_of_arguments;

        // Unpack arguments from msg->body()
        auto& args = msg->body();

        // Call method with unpacked arguments
        if constexpr (args_size == 0) {
            // No arguments
            if constexpr (std::is_void_v<result_type>) {
                // Method returns void
                (self->*method)();
                msg->set_error(mailbox::slot_error_code::ok);
            } else {
                // Method returns unique_future<T>
                using T = typename type_traits::is_unique_future<result_type>::value_type;
                result_type future = (self->*method)();

                // Link result to msg->result_slot() via continuation (non-blocking!)
                if (auto slot = msg->result_slot()) {
                    base::link_future_to_slot(std::move(future), slot);
                }
                msg->set_error(mailbox::slot_error_code::ok);
            }
        } else if constexpr (args_size == 1) {
            // One argument
            using arg_type = type_traits::type_list_at_t<args_type_list, 0>;
            using clear_arg_type = type_traits::decay_t<arg_type>;
            auto arg = args.get<clear_arg_type>(0);

            if constexpr (std::is_void_v<result_type>) {
                // Method returns void
                (self->*method)(std::forward<arg_type>(arg));
                msg->set_error(mailbox::slot_error_code::ok);
            } else {
                // Method returns unique_future<T>
                using T = typename type_traits::is_unique_future<result_type>::value_type;
                result_type future = (self->*method)(std::forward<arg_type>(arg));

                // Link result to msg->result_slot() via continuation (non-blocking!)
                if (auto slot = msg->result_slot()) {
                    base::link_future_to_slot(std::move(future), slot);
                }
                msg->set_error(mailbox::slot_error_code::ok);
            }
        } else {
            // Multiple arguments - use index sequence unpacking
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                if constexpr (std::is_void_v<result_type>) {
                    // Method returns void
                    (self->*method)((detail::get<I, args_type_list>(args))...);
                    msg->set_error(mailbox::slot_error_code::ok);
                } else {
                    // Method returns unique_future<T>
                    using T = typename type_traits::is_unique_future<result_type>::value_type;
                    result_type future = (self->*method)((detail::get<I, args_type_list>(args))...);

                    // Link result to msg->result_slot() via continuation (non-blocking!)
                    if (auto slot = msg->result_slot()) {
                        base::link_future_to_slot(std::move(future), slot);
                    }
                    msg->set_error(mailbox::slot_error_code::ok);
                }
            }(type_traits::make_index_sequence<args_size>{});
        }

        // Return ready future<void> for behavior() chaining
        return make_ready_future_void(self->resource());
    }

} // namespace actor_zeta