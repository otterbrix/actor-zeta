#pragma once

#include <actor-zeta/config.hpp>
#include <memory_resource>

namespace actor_zeta { namespace pmr {

    using std::pmr::memory_resource;

}} // namespace actor_zeta::pmr

namespace actor_zeta { namespace pmr {

    template<class T>
    using polymorphic_allocator = std::pmr::polymorphic_allocator<T>;

    using std::pmr::get_default_resource;
    using std::pmr::new_delete_resource;
    using std::pmr::null_memory_resource;
    using std::pmr::set_default_resource;

}} // namespace actor_zeta::pmr
