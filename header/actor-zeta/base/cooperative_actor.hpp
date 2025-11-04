#pragma once

#include <actor-zeta/base/forwards.hpp>

#include <actor-zeta/mailbox.hpp>
#include <actor-zeta/mailbox/default_mailbox.hpp>
#include <actor-zeta/base/detail/cooperative_actor_classic.hpp>

namespace actor_zeta { namespace base {

    /// @brief Intrusive reference counting support for cooperative_actor
    /// Works with any type that has ref() and deref() methods

    // Type trait to check if T has ref() and deref() methods
    template<typename T, typename = void>
    struct has_ref_deref : std::false_type {};

    template<typename T>
    struct has_ref_deref<T, typename std::enable_if<
        std::is_same<decltype(std::declval<T>().ref()), void>::value &&
        std::is_same<decltype(std::declval<T>().deref()), void>::value
    >::type> : std::true_type {};

    template<class T>
    auto intrusive_ptr_add_ref(T* ptr) -> typename std::enable_if<has_ref_deref<T>::value>::type {
        ptr->ref();
    }

    template<class T>
    auto intrusive_ptr_release(T* ptr) -> typename std::enable_if<has_ref_deref<T>::value>::type {
        ptr->deref();
    }

    using default_mailbox = mailbox_t<mailbox::default_mailbox_impl>;

    template<class Actor>
    using basic_actor = cooperative_actor<Actor,default_mailbox,actor_type::classic>;

}} // namespace actor_zeta::base


