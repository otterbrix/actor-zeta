#pragma once

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/config.hpp>

#include <actor-zeta/mailbox.hpp>
#include <actor-zeta/mailbox/default_mailbox.hpp>
#include <actor-zeta/base/detail/cooperative_actor_classic.hpp>

namespace actor_zeta { namespace base {

    using default_mailbox = mailbox_t<mailbox::default_mailbox_impl>;

    /// @brief Classic actor (non-coroutine)
    template<class Actor>
    using basic_actor = cooperative_actor<Actor, default_mailbox, actor_type::classic>;

}} // namespace actor_zeta::base


