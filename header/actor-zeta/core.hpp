#pragma once

// clang-format off
#include <actor-zeta/base/address.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/base/handler.hpp>
#include <actor-zeta/impl/handler.ipp>
#include <actor-zeta/future.hpp>
// clang-format on

#include <actor-zeta/base/cooperative_actor.hpp>
#include <actor-zeta/base/dispatch_traits.hpp>
#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/mailbox/id.hpp>
#include <actor-zeta/send.hpp>

namespace actor_zeta {

    using base::address_t;
    using base::make_handler;
    using base::basic_actor;
    using base::behavior_t;
    using base::make_behavior;

#if HAVE_STD_COROUTINES
    using base::coroutine_actor;
#endif

    using mailbox::message;
    using mailbox::message_ptr;
    using mailbox::message_id;
    using mailbox::make_message_id;

    using actor_zeta::unique_future;
    using actor_zeta::make_ready_future;
    using actor_zeta::make_ready_future_void;
    using actor_zeta::make_error_future;

} // namespace actor_zeta
