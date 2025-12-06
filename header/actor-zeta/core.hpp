#pragma once

// clang-format off
#include <actor-zeta/actor/address.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/future.hpp>
// clang-format on

#include <actor-zeta/actor/cooperative_actor.hpp>
#include <actor-zeta/actor/dispatch_traits.hpp>
#include <memory_resource>
#include <actor-zeta/mailbox/id.hpp>
#include <actor-zeta/send.hpp>

namespace actor_zeta {

    using actor::address_t;
    using actor::basic_actor;

    using mailbox::message;
    using mailbox::message_ptr;
    using mailbox::message_id;
    using mailbox::make_message_id;

    using actor_zeta::unique_future;
    using actor_zeta::make_ready_future;
    using actor_zeta::make_ready_future_void;
    using actor_zeta::make_error_future;

} // namespace actor_zeta