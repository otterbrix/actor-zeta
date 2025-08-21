#pragma once

#include <cassert>

#include "actor-zeta/detail/pmr/aligned_allocate.hpp"
#include "memory_resource.hpp"

namespace actor_zeta { namespace pmr {

    template<typename T>
    struct has_placement_check {
    private:
        typedef char one;
        typedef struct { char arr[2]; } two;

        template<typename U> static one test(decltype(U::placement)*);
        template<typename U> static two test(...);

    public:
        enum { value = sizeof(test<T>(nullptr)) == sizeof(one) };
    };

    template<class Target, class... Args>
    typename std::enable_if<has_placement_check<Target>::value, Target*>::type
    allocate_ptr(actor_zeta::pmr::memory_resource* resource, Args&&... args) {
        assert(resource);
        auto* buffer = resource->allocate(sizeof(Target), alignof(Target));
        auto* target_ptr = new (buffer, Target::placement) Target(std::forward<Args>(args)...);
        return target_ptr;
    }

    template<class Target, class... Args>
    typename std::enable_if<!has_placement_check<Target>::value, Target*>::type
    allocate_ptr(actor_zeta::pmr::memory_resource* resource, Args&&... args) {
        assert(resource);
        auto* buffer = resource->allocate(sizeof(Target), alignof(Target));
        auto* target_ptr = new (buffer) Target(std::forward<Args>(args)...);
        return target_ptr;
    }

    template<class Target>
    void deallocate_ptr(actor_zeta::pmr::memory_resource* resource, Target* target) {
        assert(resource);
        assert(target);
        target->~Target();
        resource->deallocate(target, sizeof(Target), alignof(Target));
    }

    class deleter_t final {
    public:
        explicit deleter_t(actor_zeta::pmr::memory_resource* resource)
            : resource_([](pmr::memory_resource* resource) {
                assert(resource);
                return resource;
            }(resource)) {}

        template<typename Target>
        void operator()(Target* target) {
            assert(target);
            assert(resource_);
            deallocate_ptr(resource_, target);
        }

    private:
        actor_zeta::pmr::memory_resource* resource_;
    };
} // namespace actor_zeta::pmr

    template<typename T, typename... Args>
    typename std::enable_if<!std::is_array<T>::value, std::unique_ptr<T>>::type
    make_unique(Args&&... args) {
        return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
    }

    // ----- Динамический массив -----
    template<typename T>
    typename std::enable_if<std::is_array<T>::value && std::extent<T>::value == 0,
                            std::unique_ptr<T>>::type
    make_unique(std::size_t n) {
        using U = typename std::remove_extent<T>::type;
        return std::unique_ptr<T>(new U[n]());
    }

    // ----- Статические массивы запрещены -----
    template<typename T, typename... Args>
    typename std::enable_if<std::extent<T>::value != 0, void>::type
    make_unique(Args&&...) = delete;

} // namespace actor_zeta