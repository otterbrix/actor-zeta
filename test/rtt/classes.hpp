#pragma once

#include <actor-zeta/detail/type_traits.hpp>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

void place(char* data, size_t capacity, size_t volume) {
    auto space_left = capacity - volume;
    printf("%s :: data: %p, capacity: %lu, volume: %lu, space_left: %lu\n",
           __func__, static_cast<void*>(data), capacity, volume, space_left);
    REQUIRE(space_left == 0);
}

template<class Head, class... Tail>
void place(char* data, size_t capacity, size_t volume, __attribute__((unused)) Head head, Tail... tail) {
    auto space_left = capacity - volume;
    void* creation_place = data + volume;
    auto aligned_place = actor_zeta::detail::align(alignof(Head), sizeof(Head), creation_place, space_left);
    printf("%s :: data: %p, capacity: %lu, volume: %lu, space_left: %lu, creation_place: %p, aligned_place: %p\n",
           __func__, static_cast<void*>(data), capacity, volume, space_left, creation_place, aligned_place);
    REQUIRE(aligned_place);
    const auto new_offset = static_cast<std::size_t>(static_cast<char*>(aligned_place) - data);
    place(data, capacity, static_cast<size_t>(new_offset + sizeof(Head)), tail...);
}

template<class... Args>
void test_getSize(Args... args) {
    auto m_capacity = actor_zeta::detail::getSize<0, Args...>();
    auto m_data(std::unique_ptr<char[]>(new char[m_capacity]));
    place(m_data.get(), m_capacity, 0, args...);
}

template<class... Args>
constexpr size_t local_getSize(Args... /*args*/) {
    return actor_zeta::detail::getSize<0, Args...>();
}

struct align_example_t {
    char a[3];
    short int b;
    long int c;
    char d[3];
};

struct X {
    int a = 0;
    std::vector<int> b = std::vector<int>{0};
    X() = default;
    X(X&) = default;
    X(const X&) = default;
    X(X&&) = default;
    ~X() = default;
    X(int a_, std::vector<int> b_)
        : a(a_)
        , b(std::move(b_)) {}
};

struct dummy {
    dummy()
        : alive(true) {
        ++created_times;
        ++instances_count;
    }

    dummy(const dummy&)
        : alive(true) {
        ++created_times;
        ++instances_count;
    }

    dummy(dummy&& other)
        : alive(true) {
        other.alive = false;
    }

    ~dummy() noexcept(false) {
        if (alive) {
            alive = false;
        } else {
            // was moved
            return;
            //throw std::runtime_error(u8"Double destruction!");
        }
        --instances_count;
        ++destroyed_times;
    }

    bool alive;
    static int instances_count;
    static int created_times;
    static int destroyed_times;
};

int dummy::instances_count = 0;
int dummy::created_times = 0;
int dummy::destroyed_times = 0;

#ifndef __EXCEPTIONS_DISABLE__

struct kamikaze {
    kamikaze() {
        ++instances_count;
    }

    kamikaze(kamikaze&) = default;
    kamikaze(const kamikaze&) = default;

    kamikaze(kamikaze&&) {
        if (instances_count > 4) {
            throw std::runtime_error(u8"Hello!");
        }
        ++instances_count;
    }

    ~kamikaze() {
        --instances_count;
    }

    static int instances_count;
};

int kamikaze::instances_count = 0;

struct throw_on_move {
    throw_on_move() = default;
    throw_on_move(const throw_on_move&) = default;
    throw_on_move& operator=(const throw_on_move&) = default;
    throw_on_move(throw_on_move&&) {
        throw std::runtime_error(u8"Goodbye!");
    }
    throw_on_move& operator=(throw_on_move&&) = default;
    ~throw_on_move() = default;
};

#endif

struct move_constructor_counter {
    explicit move_constructor_counter(std::size_t& move_count)
        : move_count(move_count) {
    }

    move_constructor_counter(move_constructor_counter&& that)
        : move_count(that.move_count) {
        ++move_count;
    }

    std::size_t& move_count;
};

struct destructor_counter {
    explicit destructor_counter(std::size_t& destruct_count)
        : destruct_count(destruct_count) {
    }

    ~destructor_counter() {
        ++destruct_count;
    }

    std::size_t& destruct_count;
};


struct movable_only {
    movable_only(){}
    movable_only(const movable_only&) = delete;
    movable_only& operator=(const movable_only&) = delete;
    movable_only(movable_only&& other){
        alive = other.alive;
        other.alive = false;
        assert(alive);
    }
    movable_only& operator=(movable_only&& other){
        alive = other.alive;
        other.alive = false;
        assert(alive);
    }

    bool alive {true};
};

template<typename... Ts>
void ignore_unused(Ts const&...) {}

template<typename... Ts>
void ignore_unused() {}
