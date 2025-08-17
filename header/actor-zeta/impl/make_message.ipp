#pragma once

// clang-format off
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/base/address.hpp>
#include <actor-zeta/mailbox/message.hpp>
// clang-format on

namespace actor_zeta {

    auto make_message(actor_zeta::pmr::memory_resource*resource,base::address_t sender_, mailbox::message_id id) -> mailbox::message_ptr {
        return mailbox::message_ptr(new mailbox::message(resource,std::move(sender_),id));
    }

} // namespace actor_zeta