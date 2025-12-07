#pragma once

// clang-format off
#include <actor-zeta/actor/address.hpp>
#include <actor-zeta/mailbox/message.hpp>
// clang-format on

#include <memory>

namespace actor_zeta { namespace actor {

    address_t::address_t() noexcept
        : ptr_(nullptr) {
    }

    address_t::address_t(void* ptr)
        : ptr_([](void* ptr) {assert(ptr!=nullptr);return ptr; }(ptr)) {}

    bool address_t::operator!() const noexcept {
        return !(static_cast<bool>(ptr_));
    }

    address_t::operator bool() const noexcept {
        return static_cast<bool>(ptr_);
    }

    void* address_t::get() const noexcept {
        return ptr_;
    }

    address_t::address_t(address_t&& other) noexcept {
        swap(other);
    }

    address_t::address_t(const address_t& other) {
        if (this != &other) {
            ptr_ = other.ptr_;
        }
    }

    address_t& address_t::operator=(address_t&& other) noexcept {
        if (this != &other) {
            swap(other);
        }
        return *this;
    }

    address_t& address_t::operator=(const address_t& other) {
        if (this != &other) {
            address_t tmp(other);
            swap(tmp);
        }
        return *this;
    }

    void address_t::swap(address_t& other) {
        using std::swap;
        std::swap(ptr_, other.ptr_);
    }

    address_t::~address_t() noexcept {
        ptr_ = nullptr;
    }

    auto address_t::empty_address() -> address_t {
        static address_t tmp;
        return tmp;
    }

    bool address_t::operator==(const address_t& rhs) noexcept {
        return ptr_ == rhs.ptr_;
    }

}} // namespace actor_zeta::actor