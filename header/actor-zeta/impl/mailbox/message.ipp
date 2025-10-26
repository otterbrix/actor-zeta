#pragma once

#include <utility>

#include <actor-zeta/base/address.hpp>
#include <actor-zeta/mailbox/message.hpp>

namespace actor_zeta { namespace mailbox {

    auto message::command() const noexcept -> message_id {
        return command_;
    }

    message::operator bool() {
        return command_.integer_value() != 0 || bool(sender_) || !body_.empty();
    }

    message::message(actor_zeta::pmr::memory_resource* resource, address_t sender, message_id name)
        : sender_(std::move(sender))
        , command_(std::move(name))
        , body_(resource)
        , result_slot_(nullptr) {}

    message::message(actor_zeta::pmr::memory_resource* resource, address_t sender, message_id name, actor_zeta::detail::rtt&& body)
        : sender_(std::move(sender))
        , command_(std::move(name))
        , body_(std::allocator_arg, resource, std::move(body))
        , result_slot_(nullptr) {}

    message::message(message&& other) noexcept
        : sender_(std::move(other.sender_))
        , command_(std::move(other.command_))
        , body_(std::move(other.body_))
        , result_slot_(other.result_slot_)
        , error_(other.error_.load(std::memory_order_acquire))
        , cancelled_(other.cancelled_.load(std::memory_order_acquire))
        , orphaned_(other.orphaned_.load(std::memory_order_acquire)) {
    }

    message::message(std::allocator_arg_t, actor_zeta::pmr::memory_resource* resource, message&& other) noexcept
           : sender_(std::move(other.sender_))
           , command_(std::move(other.command_))
           , body_(std::allocator_arg, resource, std::move(other.body_))
           , result_slot_(other.result_slot_)
           , error_(other.error_.load(std::memory_order_acquire))
           , cancelled_(other.cancelled_.load(std::memory_order_acquire))
           , orphaned_(other.orphaned_.load(std::memory_order_acquire)) {
    }

    message& message::operator=(message&& other) noexcept {
        if (this == &other) return *this;

        sender_ = std::move(other.sender_);
        command_ = std::move(other.command_);
        body_ = std::move(other.body_);
        result_slot_ = other.result_slot_;
        error_.store(other.error_.load(std::memory_order_acquire), std::memory_order_release);
        cancelled_.store(other.cancelled_.load(std::memory_order_acquire), std::memory_order_release);
        orphaned_.store(other.orphaned_.load(std::memory_order_acquire), std::memory_order_release);

        return *this;
    }

    message::message(actor_zeta::pmr::memory_resource* resource)
        : singly_linked(nullptr)
        , prev(nullptr)
        , sender_(address_t::empty_address())
        , body_(resource)
        , result_slot_(nullptr) {}

    message::~message() noexcept {}

    void message::swap(message& other) noexcept {
        using std::swap;
        swap(sender_, other.sender_);
        swap(command_, other.command_);
        swap(body_, other.body_);
        swap(result_slot_, other.result_slot_);

        // Swap atomic fields
        auto this_error = error_.load(std::memory_order_acquire);
        auto other_error = other.error_.load(std::memory_order_acquire);
        error_.store(other_error, std::memory_order_release);
        other.error_.store(this_error, std::memory_order_release);

        auto this_cancelled = cancelled_.load(std::memory_order_acquire);
        auto other_cancelled = other.cancelled_.load(std::memory_order_acquire);
        cancelled_.store(other_cancelled, std::memory_order_release);
        other.cancelled_.store(this_cancelled, std::memory_order_release);

        auto this_orphaned = orphaned_.load(std::memory_order_acquire);
        auto other_orphaned = other.orphaned_.load(std::memory_order_acquire);
        orphaned_.store(other_orphaned, std::memory_order_release);
        other.orphaned_.store(this_orphaned, std::memory_order_release);
    }

    bool message::is_high_priority() const {
        return command_.priority() == detail::high_message_priority;
    }

    auto message::body() -> actor_zeta::detail::rtt& {
        assert(!body_.empty());
        return body_;
    }

    auto message::sender() & noexcept -> base::address_t& {
        return sender_;
    }

    auto message::sender() && noexcept -> base::address_t&& {
        return std::move(sender_);
    }

    auto message::sender() const& noexcept -> base::address_t const& {
        return sender_;
    }

}} // namespace actor_zeta::mailbox
