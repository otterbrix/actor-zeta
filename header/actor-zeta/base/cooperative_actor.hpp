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

#if HAVE_STD_COROUTINES
    /// @brief Coroutine-enabled actor (C++20)
    /// @note Requires HAVE_STD_COROUTINES to be enabled
    /// @note Implementation is IDENTICAL to basic_actor - all coroutine logic in behavior_t
    template<class Actor>
    using coroutine_actor = cooperative_actor<Actor, default_mailbox, actor_type::coroutine>;
#endif // HAVE_STD_COROUTINES

}} // namespace actor_zeta::base


