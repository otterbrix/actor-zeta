#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <type_traits>

#include "controlled_allocators.hpp"

#include <actor-zeta/detail/memory.hpp>
#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/pmr/polymorphic_allocator.hpp>
#include <actor-zeta/detail/type_traits.hpp>

using actor_zeta::pmr::memory_resource;
using actor_zeta::pmr::polymorphic_allocator;

template<class provider_t, int N>
class test_resource_impl_t : public memory_resource {
public:
    static int resource_alive;
    static int resource_constructed;
    static int resource_destructed;

    static void reset_statics() {
        assert(resource_alive == 0);
        resource_alive = 0;
        resource_constructed = 0;
        resource_destructed = 0;
    }

    using memory_resource = memory_resource;
    using provider = provider_t;

    int value;
    int type;

    explicit test_resource_impl_t(int val = 0)
        : value(val)
        , type(N) {
        ++resource_alive;
        ++resource_constructed;
    }

    ~test_resource_impl_t() noexcept {
        --resource_alive;
        ++resource_destructed;
    }

    void reset() {
        C.reset();
        P.reset();
    }
    alloc_controller_t& get_controller() { return C; }

    bool check_alloc(void* p, std::size_t s, std::size_t a) const { return C.check_alloc(p, s, a); }

    bool check_dealloc(void* p, std::size_t s, std::size_t a) const { return C.check_dealloc(p, s, a); }

    bool check_is_equal_called_eq(int n) const { return C.check_is_equal_called_eq(n); }

protected:
    virtual void* do_allocate(std::size_t s, std::size_t a) {
        if (C.throw_on_alloc) {
#ifndef TEST_HAS_NO_EXCEPTIONS
            throw test_exception_t{};
#else
            assert(false);
#endif
        }
        void* ret = P.allocate(s, a);
        C.count_alloc(ret, s, a);
        return ret;
    }

    virtual void do_deallocate(void* p, std::size_t s, std::size_t a) {
        C.count_dealloc(p, s, a);
        P.deallocate(p, s, a);
    }

    virtual bool do_is_equal(memory_resource const& other) const noexcept {
        C.count_is_equal();
        test_resource_impl_t<provider_t, N> const* o = static_cast<test_resource_impl_t<provider_t, N> const*>(&other);
        return o && o->value == value && o->type == type;
    }

private:
    mutable alloc_controller_t C;
    mutable provider P;
    DISALLOW_COPY(test_resource_impl_t);
};

template<class provider, int N>
int test_resource_impl_t<provider, N>::resource_alive = 0;

template<class provider, int N>
int test_resource_impl_t<provider, N>::resource_constructed = 0;

template<class provider, int N>
int test_resource_impl_t<provider, N>::resource_destructed = 0;

struct null_provider_t {
    null_provider_t() {}
    void* allocate(size_t, size_t) { return nullptr; }
    void deallocate(void*, size_t, size_t) {}
    void reset() {}

private:
    DISALLOW_COPY(null_provider_t);
};

struct new_delete_provider_t {
    new_delete_provider_t() {}
    void* allocate(size_t s, size_t) { return ::operator new(s); }
    void deallocate(void* p, size_t, size_t) { ::operator delete(p); }
    void reset() {}

private:
    DISALLOW_COPY(new_delete_provider_t);
};

template<size_t Size = 4096 * 10> // 10 pages worth of memory.
struct buffer_provider_t {
    char buffer[Size];
    void* next = &buffer;
    size_t space = Size;

    buffer_provider_t() {}

    void* allocate(size_t s, size_t a) {
        void* ret = actor_zeta::detail::align(s, a, next, space);
        if (ret == nullptr) {
#ifndef TEST_HAS_NO_EXCEPTIONS
            throw std::bad_alloc();
#else
            assert(false);
#endif
        }

        return ret;
    }

    void deallocate(void*, size_t, size_t) {}

    void reset() {
        next = &buffer;
        space = Size;
    }

private:
    DISALLOW_COPY(buffer_provider_t);
};

using null_resource_t = test_resource_impl_t<null_provider_t, 0>;
using new_delete_resource_t = test_resource_impl_t<new_delete_provider_t, 0>;
using test_resource_t = test_resource_impl_t<buffer_provider_t<>, 0>;
using test_resource1_t = test_resource_impl_t<buffer_provider_t<>, 1>;
using test_resource2_t = test_resource_impl_t<buffer_provider_t<>, 2>;
