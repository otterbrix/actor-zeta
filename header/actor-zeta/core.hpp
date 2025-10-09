#pragma once

// clang-format off
#include <actor-zeta/base/address.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/base/actor_abstract.hpp>
#include <actor-zeta/base/handler.hpp>
#include <actor-zeta/impl/handler.ipp>
// clang-format on

#include <actor-zeta/base/cooperative_actor.hpp>
#include <actor-zeta/detail/pmr/memory_resource.hpp>
#include <actor-zeta/mailbox/id.hpp>

namespace actor_zeta {

    using base::actor_abstract_t;
    using base::address_t;
    using base::make_handler;
    using base::basic_actor;
    using base::behavior_t;
    using base::make_behavior;

    using mailbox::message;
    using mailbox::message_ptr;
    using mailbox::message_id;
    using mailbox::make_message_id;

    using scheduler::scheduler_t;
    
} // namespace actor_zeta
