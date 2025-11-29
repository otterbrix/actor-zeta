#pragma once

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/future.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/detail/rtt.hpp>
#include <iostream>  // for debug output

namespace actor_zeta {

namespace detail {

    /// @brief Link future result to message result_slot via continuation (NON-BLOCKING)
    /// @param source Future from method call (e.g., (ptr->*f)(args...))
    /// @param target_slot Message's result_slot to receive result
    /// @note Uses continuation mechanism - NEVER calls .get() on suspended futures
    /// @note Thread-safe: CAS protection in set_result_rtt()
    ///
    /// @details
    /// FAST PATH (source ready):
    ///   - Use virtual propagate_result_to() to transfer result
    ///   - NO blocking
    ///
    /// SLOW PATH (source not ready):
    ///   - Set continuation so result propagates when ready
    ///   - NO blocking
    template<typename T>
    void link_future_to_slot(
        unique_future<T>& source,
        intrusive_ptr<detail::future_state_base> target_slot
    ) noexcept {
        auto* source_ptr = source.get_state();
        std::cerr << "[link_future_to_slot] source=" << static_cast<void*>(source_ptr)
                  << " target=" << static_cast<void*>(target_slot.get())
                  << " source.is_ready()=" << source.is_ready() << std::endl;

        // FAST PATH: Already ready - transfer result immediately
        if (source.is_ready()) {
            std::cerr << "[link_future_to_slot] FAST PATH: source ready, propagating result..." << std::endl;

            auto* source_state = source.get_state();
            if (!source_state) {
                std::cerr << "[link_future_to_slot] FAST PATH: source has no state, skipping" << std::endl;
                return;
            }

            // Use virtual method - source_state knows its type and can copy result
            source_state->propagate_result_to(target_slot.get());

            std::cerr << "[link_future_to_slot] FAST PATH done" << std::endl;
            return;
        }

        // SLOW PATH: NOT ready - set continuation (NO BLOCKING!)
        std::cerr << "[link_future_to_slot] SLOW PATH: source NOT ready, setting continuation" << std::endl;
        auto* source_state = source.get_state();

        // set_continuation() increments source_state refcount to keep it alive
        // When source becomes ready, set_result_rtt() will propagate result to target_slot
        source_state->set_continuation(target_slot);

        std::cerr << "[link_future_to_slot] SLOW PATH done, continuation set" << std::endl;
    }

} // namespace detail

    /// @brief Dispatch message to actor method (NEW API)
    /// @tparam Actor Actor type (deduced from method pointer)
    /// @tparam Method Method pointer type
    /// @param self Actor instance pointer
    /// @param method Method pointer (e.g., &Actor::compute)
    /// @param msg Message with arguments in body()
    /// @return unique_future<T> where T is the method's return type
    ///
    /// @details
    /// dispatch() does:
    /// 1. Unpack arguments from msg->body() using callable_trait
    /// 2. Call (self->*method)(args...)
    /// 3. If method returns unique_future<T>:
    ///    - Links result to msg->result_slot() (if present)
    /// 4. Returns the future from the method
    ///
    /// Usage inside actor's behavior():
    /// @code
    /// return dispatch(this, &Actor::compute, msg);
    /// @endcode
    template<class Actor, typename Method>
    auto dispatch(Actor* self, Method method, mailbox::message* msg) {
        using call_trait = type_traits::get_callable_trait_t<Method>;
        using result_type = typename call_trait::result_type;
        using args_type_list = typename call_trait::args_types;
        constexpr int args_size = call_trait::number_of_arguments;

        // Helper lambda to link future to result_slot (while T is known)
        auto link_result = [msg](auto& future) {
            if (auto slot = msg->result_slot()) {
                std::cerr << "[dispatch] Linking future to result_slot, T is known here!" << std::endl;
                detail::link_future_to_slot(future, slot);
            }
        };

        // Call method with unpacked arguments
        if constexpr (args_size == 0) {
            // No arguments - DON'T call msg->body()!
            if constexpr (std::is_void_v<result_type>) {
                // Method returns void
                (self->*method)();
                return make_ready_future_void(self->resource());
            } else {
                // Method returns unique_future<T>
                auto future = (self->*method)();
                link_result(future);  // Link while T is known!
                return future;
            }
        } else if constexpr (args_size == 1) {
            // Unpack arguments from msg->body()
            auto& args = msg->body();
            // One argument
            using arg_type = type_traits::type_list_at_t<args_type_list, 0>;
            using clear_arg_type = type_traits::decay_t<arg_type>;
            auto arg = args.get<clear_arg_type>(0);

            if constexpr (std::is_void_v<result_type>) {
                // Method returns void
                (self->*method)(std::forward<arg_type>(arg));
                return make_ready_future_void(self->resource());
            } else {
                // Method returns unique_future<T>
                auto future = (self->*method)(std::forward<arg_type>(arg));
                std::cerr << "[dispatch] Returned future from method, state=" << static_cast<void*>(future.get_state()) << std::endl;
                link_result(future);  // Link while T is known!
                return future;
            }
        } else {
            // Multiple arguments - use index sequence unpacking
            auto& args = msg->body();
            return [&]<std::size_t... I>(std::index_sequence<I...>) {
                if constexpr (std::is_void_v<result_type>) {
                    // Method returns void
                    (self->*method)((detail::get<I, args_type_list>(args))...);
                    return make_ready_future_void(self->resource());
                } else {
                    // Method returns unique_future<T>
                    auto future = (self->*method)((detail::get<I, args_type_list>(args))...);
                    link_result(future);  // Link while T is known!
                    return future;
                }
            }(type_traits::make_index_sequence<args_size>{});
        }
    }

} // namespace actor_zeta