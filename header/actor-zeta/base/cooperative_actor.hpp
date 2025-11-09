#pragma once

#include <actor-zeta/base/forwards.hpp>

#include <actor-zeta/mailbox.hpp>
#include <actor-zeta/mailbox/default_mailbox.hpp>
#include <actor-zeta/base/detail/cooperative_actor_classic.hpp>

namespace actor_zeta { namespace base {

    template<class T,class Target,class Traits,class Type>
    auto intrusive_ptr_add_ref(T* ptr) -> typename std::enable_if<std::is_same<T*, cooperative_actor<Target,Traits,Type>*>::value>::type {
        ptr->intrusive_ptr_add_ref_impl();
    }

    template<class T,class Target,class Traits,class Type>
    auto intrusive_ptr_release(T* ptr) -> typename std::enable_if<std::is_same<T*, cooperative_actor<Target,Traits,Type>*>::value>::type {
        ptr->intrusive_ptr_release_impl();
    }

    using default_mailbox = mailbox_t<mailbox::default_mailbox_impl>;

    template<class Actor>
    using basic_actor = cooperative_actor<Actor,default_mailbox,actor_type::classic>;

}} // namespace actor_zeta::base


