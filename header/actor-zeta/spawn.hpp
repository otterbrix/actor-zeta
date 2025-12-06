#pragma once
#include <memory_resource>
#include <actor-zeta/detail/memory.hpp>

namespace actor_zeta {

    template<
    class Target,
    class... Args>
std::unique_ptr<Target, actor_zeta::pmr::deleter_t> spawn(std::pmr::memory_resource* resource, Args&&... args) noexcept {
        using type = typename std::decay<Target>::type;
        auto* target_ptr = actor_zeta::pmr::allocate_ptr<type>(resource,resource, std::forward<Args&&>(args)...);
        return {target_ptr, actor_zeta::pmr::deleter_t{resource}};
    }


    template<class Target>
    std::unique_ptr<Target, actor_zeta::pmr::deleter_t> spawn(std::pmr::memory_resource* resource) noexcept {
        using type = typename std::decay<Target>::type;
        auto* target_ptr = actor_zeta::pmr::allocate_ptr<type>(resource,resource);
        return {target_ptr, actor_zeta::pmr::deleter_t{resource}};
    }

}