#pragma once

#include <actor-zeta/detail/future.hpp>
#include <actor-zeta/detail/generator.hpp>
#include <actor-zeta/mailbox/message.hpp>

#include <actor-zeta/detail/type_traits.hpp>

namespace actor_zeta { namespace mailbox {

    template<typename T>
    [[nodiscard]] actor_zeta::promise<T> message::get_result_promise() const noexcept {
        auto* typed_state = static_cast<actor_zeta::detail::future_state<T>*>(result_slot_.get());
        return actor_zeta::promise<T>(
            type_traits::internal_construct_tag{},
            typed_state,
            typed_state->memory_resource());
    }

    template<typename T>
    [[nodiscard]] actor_zeta::detail::generator_state<T>* message::get_generator_state() const noexcept {
        return static_cast<actor_zeta::detail::generator_state<T>*>(result_slot_.get());
    }

}} // namespace actor_zeta::mailbox