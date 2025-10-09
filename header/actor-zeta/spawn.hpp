#pragma once
#include <actor-zeta/detail/memory.hpp>
#include <actor-zeta/detail/memory_resource.hpp>

namespace actor_zeta {

    template<
    class Target,
    class... Args>
std::unique_ptr<Target, actor_zeta::pmr::deleter_t> spawn(actor_zeta::pmr::memory_resource* resource, Args&&... args) noexcept {
        using type = typename std::decay<Target>::type;
        auto* target_ptr = actor_zeta::pmr::allocate_ptr<type>(resource,resource, std::forward<Args&&>(args)...);
        return {target_ptr, actor_zeta::pmr::deleter_t{resource}};
    }


    template<class Target>
    std::unique_ptr<Target, actor_zeta::pmr::deleter_t> spawn(actor_zeta::pmr::memory_resource* resource) noexcept {
        using type = typename std::decay<Target>::type;
        auto* target_ptr = actor_zeta::pmr::allocate_ptr<type>(resource,resource);
        return {target_ptr, actor_zeta::pmr::deleter_t{resource}};
    }

}