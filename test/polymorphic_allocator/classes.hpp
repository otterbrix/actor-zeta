#pragma once

#include <cstddef>

#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/type_traits.hpp>

#include "test_memory_resource.hpp"

struct destroyable {
    static int count;
    destroyable() { ++count; }
    ~destroyable() { --count; }
};

int destroyable::count = 0;

struct default_constructible {
    static int constructed;
    default_constructible()
        : x(42) { ++constructed; }
    int x{0};
};

int default_constructible::constructed = 0;

template<class T>
struct test_harness_t {
    test_resource_t R;
    actor_zeta::pmr::memory_resource* M = &R;
    actor_zeta::pmr::polymorphic_allocator<T> A = M;
    bool constructed = false;
    T* ptr;
    test_harness_t()
        : ptr(A.allocate(1)) {}
    template<class... Args>
    void construct(Args&&... args) {
        A.construct(ptr, std::forward<Args>(args)...);
        constructed = true;
    }
    ~test_harness_t() {
        if (constructed)
            A.destroy(ptr);
        A.deallocate(ptr, 1);
    }
};

struct count_copies_t {
    static int count;
    bool copied = false;
    count_copies_t() {}
    count_copies_t(count_copies_t const&) {
        count_copies_t::count++;
        copied = true;
        printf("%s :: %d\n", __func__, count_copies_t::count);
    }
    ~count_copies_t() {
        if (copied) {
            count_copies_t::count--;
        }
        printf("~%s :: %d\n", __func__, count_copies_t::count);
    }
};

int count_copies_t::count = 0;

struct count_copies_alloc_v1_t {
    typedef actor_zeta::pmr::polymorphic_allocator<char> allocator_type;
    actor_zeta::pmr::memory_resource* alloc;
    static int count;
    bool copied = false;
    count_copies_alloc_v1_t()
        : alloc(nullptr) {
        printf("%s :: %d\n", __func__, count_copies_alloc_v1_t::count);
    }
    count_copies_alloc_v1_t(
        actor_zeta::type_traits::allocator_arg_t, allocator_type const& a,
        count_copies_alloc_v1_t const&)
        : alloc(a.resource()) {
        copied = true;
        count_copies_alloc_v1_t::count++;
        printf("%s :: %d\n", __func__, count_copies_alloc_v1_t::count);
    }
    count_copies_alloc_v1_t(
        allocator_type const& a,
        count_copies_alloc_v1_t const& o)
        : alloc(a.resource()) {
        copied = true;
        count_copies_alloc_v1_t::count++;
        printf("%s :: %d\n", __func__, count_copies_alloc_v1_t::count);
    }
    count_copies_alloc_v1_t(
        count_copies_alloc_v1_t const&,
        allocator_type const& a)
        : alloc(a.resource()) {
        copied = true;
        count_copies_alloc_v1_t::count++;
        printf("%s :: %d\n", __func__, count_copies_alloc_v1_t::count);
    }
    count_copies_alloc_v1_t(count_copies_alloc_v1_t const& o) {
        copied = true;
        count_copies_alloc_v1_t::count++;
        printf("%s :: %d\n", __func__, count_copies_alloc_v1_t::count);
    }
    ~count_copies_alloc_v1_t() {
        if (copied) {
            count_copies_alloc_v1_t::count--;
        }
        printf("~%s :: %d\n", __func__, count_copies_alloc_v1_t::count);
    }
};

int count_copies_alloc_v1_t::count = 0;

struct count_copies_alloc_v2_t {
    typedef actor_zeta::pmr::polymorphic_allocator<char> allocator_type;
    actor_zeta::pmr::memory_resource* alloc;
    static int count;
    bool copied = false;
    count_copies_alloc_v2_t()
        : alloc(nullptr) {
    }
    count_copies_alloc_v2_t(
        count_copies_alloc_v2_t const& o, allocator_type const& a)
        : alloc(a.resource()) {
        copied = true;
        count_copies_alloc_v2_t::count++;
    }
    count_copies_alloc_v2_t(count_copies_alloc_v2_t const& o) {
        copied = true;
        count_copies_alloc_v2_t::count++;
    }
    ~count_copies_alloc_v2_t() {
        if (copied) {
            count_copies_alloc_v2_t::count--;
        }
    }
};

int count_copies_alloc_v2_t::count = 0;

class x {
public:
    x() = default;
    x(int value)
        : value_(value) {}
    ~x() = default;
    int value_ = 0;
};

using test_type = actor_zeta::pmr::polymorphic_allocator<x>;

class derived_from_memory_resource
    : public actor_zeta::pmr::memory_resource {
public:
    explicit derived_from_memory_resource(unsigned i = 0u)
        : id(i) {}

    virtual ~derived_from_memory_resource() { destructor_called = true; }

    virtual void* do_allocate(std::size_t bytes, std::size_t alignment) {
        do_allocate_called = true;
        do_allocate_bytes = bytes;
        do_allocate_alignment = alignment;
        return do_allocate_return;
    }

    virtual void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) {
        do_deallocate_called = true;
        do_deallocate_p = p;
        do_deallocate_bytes = bytes;
        do_deallocate_alignment = alignment;
    }

    virtual bool do_is_equal(const actor_zeta::pmr::memory_resource& other) const noexcept {
        do_is_equal_called = true;
        do_is_equal_other = &other;
        return static_cast<const derived_from_memory_resource&>(other).id == this->id;
    }

    void reset() {
        destructor_called = false;

        do_allocate_return = 0;
        do_allocate_called = false;
        do_allocate_bytes = 0u;
        do_allocate_alignment = 0u;

        do_deallocate_called = false;
        do_deallocate_p = 0;
        do_deallocate_bytes = 0u;
        do_deallocate_alignment = 0u;

        do_is_equal_called = false;
        do_is_equal_other = 0;
    }

    static bool destructor_called;

    unsigned id;
    void* do_allocate_return;
    mutable bool do_allocate_called;
    mutable std::size_t do_allocate_bytes;
    mutable std::size_t do_allocate_alignment;

    mutable bool do_deallocate_called;
    mutable void* do_deallocate_p;
    mutable std::size_t do_deallocate_bytes;
    mutable std::size_t do_deallocate_alignment;

    mutable bool do_is_equal_called;
    mutable const actor_zeta::pmr::memory_resource* do_is_equal_other;
};

bool derived_from_memory_resource::destructor_called = false;

struct char_holder {
    char m_char;
    ~char_holder() { destructor_called = true; }
    static bool destructor_called;
};

bool char_holder::destructor_called = false;
