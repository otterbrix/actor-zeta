#pragma once

#include <cassert>

#include <memory_resource>

namespace actor_zeta { namespace pmr {

    template<typename T>
    struct has_placement_check {
    private:
        typedef char one;
        typedef struct {
            char arr[2];
        } two;

        template<typename U>
        static one test(decltype(U::placement)*);
        template<typename U>
        static two test(...);

    public:
        enum { value = sizeof(test<T>(nullptr)) == sizeof(one) };
    };

    template<class Target, class... Args>
    typename std::enable_if<has_placement_check<Target>::value, Target*>::type
    allocate_ptr(std::pmr::memory_resource* resource, Args&&... args) {
        assert(resource);
        auto* buffer = resource->allocate(sizeof(Target), alignof(Target));
        auto* target_ptr = new (buffer, Target::placement) Target(std::forward<Args>(args)...);
        return target_ptr;
    }

    template<class Target, class... Args>
    typename std::enable_if<!has_placement_check<Target>::value, Target*>::type
    allocate_ptr(std::pmr::memory_resource* resource, Args&&... args) {
        assert(resource);
        auto* buffer = resource->allocate(sizeof(Target), alignof(Target));
        auto* target_ptr = new (buffer) Target(std::forward<Args>(args)...);
        return target_ptr;
    }

    template<class Target>
    void deallocate_ptr(std::pmr::memory_resource* resource, Target* target) {
        assert(resource);
        assert(target);
        target->~Target();
        resource->deallocate(target, sizeof(Target), alignof(Target));
    }

    class deleter_t final {
    public:
        explicit deleter_t(std::pmr::memory_resource* resource)
            : resource_([](std::pmr::memory_resource* resource) {
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
        std::pmr::memory_resource* resource_;
    };
}} // namespace actor_zeta::pmr