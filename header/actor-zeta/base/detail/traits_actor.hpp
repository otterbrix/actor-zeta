#pragma once

#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/mailbox.hpp>
#include <actor-zeta/mailbox/default_mailbox.hpp>

namespace actor_zeta { namespace base {

    struct traits {
        using mailbox_t = mailbox_t<mailbox::default_mailbox_impl>;
    };


    namespace actor_type {
        class classic;
        class coroutine;
    }

}} // namespace actor_zeta::base