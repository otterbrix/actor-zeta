#pragma once
#include "actor-zeta/mailbox/default_mailbox.hpp"
#include "cooperative_actor.hpp"

namespace actor_zeta { namespace actor {

    template<class Actor>
    using basic_actor = cooperative_actor<Actor, actor_zeta::mailbox::default_mailbox>;

}} // namespace actor_zeta::actor