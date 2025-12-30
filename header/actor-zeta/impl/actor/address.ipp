#pragma once

// clang-format off
#include <actor-zeta/actor/address.hpp>
#include <actor-zeta/mailbox/message.hpp>
// clang-format on

#include <memory>

namespace actor_zeta { namespace actor {

    address_t::address_t() noexcept
        : resource_(nullptr)
        , ptr_(nullptr)
        , enqueue_fn_(nullptr) {
    }

    address_t::address_t(std::pmr::memory_resource* resource, void* ptr)
        : resource_([](std::pmr::memory_resource* r){assert(r!=nullptr);return r;}(resource))
        , ptr_([](void* p){assert(p!=nullptr);return p;}(ptr)) {
    }

    bool address_t::operator!() const noexcept {
        return !(static_cast<bool>(ptr_));
    }

    address_t::operator bool() const noexcept {
        return static_cast<bool>(ptr_);
    }

    void* address_t::get() const noexcept {
        return ptr_;
    }

    address_t::address_t(address_t&& other) noexcept
        : resource_(nullptr)
        , ptr_(nullptr)
        , enqueue_fn_(nullptr) {
        swap(other);
    }

    address_t::address_t(const address_t& other)
        : resource_(other.resource_)
        , ptr_(other.ptr_)
        , enqueue_fn_(other.enqueue_fn_) {
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
        std::swap(resource_, other.resource_);
        std::swap(enqueue_fn_, other.enqueue_fn_);
    }

    std::pair<detail::enqueue_result, bool> address_t::enqueue_impl(mailbox::message_ptr msg) const {
        assert(enqueue_fn_ && "enqueue_fn_ is null - address was not created from actor");
        return enqueue_fn_(ptr_, msg.release());
    }

    std::pmr::memory_resource* address_t::resource() const noexcept {
        return resource_;
    }

    address_t::~address_t() noexcept {
        ptr_ = nullptr;
    }

    auto address_t::empty_address() -> address_t {
        static address_t tmp;
        return tmp;
    }

    bool address_t::operator==(const address_t& rhs) const noexcept {
        return ptr_ == rhs.ptr_;
    }

}} // namespace actor_zeta::actor