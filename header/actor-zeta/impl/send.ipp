#pragma once

#include <actor-zeta/send.hpp>

namespace actor_zeta {
    void send(base::actor_abstract_t* actor, message_ptr ptr) {
        actor->enqueue(std::move(ptr));
    }

    void enqueue(actor_zeta::scheduler_t* scheduler, actor_zeta::scheduler::resumable_t* ptr) {
        scheduler->schedule(ptr);
    }

} // namespace actor_zeta