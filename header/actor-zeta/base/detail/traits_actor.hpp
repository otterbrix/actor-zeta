#pragma once

#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/queue/fifo_inbox.hpp>
#include <actor-zeta/mailbox.hpp>

namespace actor_zeta { namespace base {

    struct traits {
        using inbox_t = detail::fifo_inbox<mailbox_policy>;
    };

    inline auto high(traits::inbox_t& inbox) -> high_priority_queue& {
        return std::get<high_priority_queue_index>(inbox.queue().queues());
    }

    inline auto normal(traits::inbox_t& inbox) -> normal_priority_queue& {
        return std::get<normal_priority_queue_index>(inbox.queue().queues());
    }

    namespace actor_type {
        class classic;
        class coroutine;
    }

}} // namespace actor_zeta::base