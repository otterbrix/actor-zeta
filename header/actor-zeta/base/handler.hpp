#pragma once

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/detail/unique_function.hpp>

namespace actor_zeta { namespace base {

    using action = detail::unique_function<void(mailbox::message*)>;

    template<typename F>
    auto make_handler(actor_zeta::pmr::memory_resource* resource, F&& f) -> action;

    template<typename ClassPtr, typename F>
    auto make_handler(actor_zeta::pmr::memory_resource* resource, ClassPtr* self, F&& f) -> action;

}} // namespace actor_zeta::base
