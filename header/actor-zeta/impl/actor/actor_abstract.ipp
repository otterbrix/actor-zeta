#pragma once

// clang-format off
#include <actor-zeta/base/handler.hpp>
#include <actor-zeta/base/address.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/base/actor_abstract.hpp>
#include <actor-zeta/impl/handler.ipp>
// clang-format on

#include <iostream>

namespace actor_zeta { namespace base {

    actor_abstract_t::actor_abstract_t(pmr::memory_resource* resource)
        : resource_([](pmr::memory_resource* resource) {assert(resource);return resource; }(resource)) {}

    actor_abstract_t::~actor_abstract_t() {}

    address_t actor_abstract_t::address() noexcept {
        return address_t(this);
    }

    void actor_abstract_t::operator delete(void*) noexcept{

    }

    auto actor_abstract_t::id() const -> id_t {
        return id_t{const_cast<actor_abstract_t*>(this)};
    }

    actor_zeta::pmr::memory_resource* actor_abstract_t::resource() const noexcept {
        return resource_;
    }

}} // namespace actor_zeta::base
