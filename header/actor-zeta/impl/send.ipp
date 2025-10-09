#pragma once

#include <actor-zeta/send.hpp>

namespace actor_zeta {
    void send(base::actor_abstract_t* actor, message_ptr ptr) {
        actor->enqueue(std::move(ptr));
    }

} // namespace actor_zeta