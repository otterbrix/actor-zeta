#pragma once

#include <actor-zeta/detail/queue/enqueue_result.hpp>
#include <actor-zeta/mailbox/default_mailbox.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/mailbox/message_result.hpp>

namespace actor_zeta::mailbox {

    using mailbox::make_message_id;
    using mailbox::message;
    using mailbox::message_id;
    using mailbox::message_ptr;

} // namespace actor_zeta::mailbox