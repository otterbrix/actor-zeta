#pragma once

#include <utility>

#include <actor-zeta/actor/address.hpp>
#include <actor-zeta/mailbox/message.hpp>

namespace actor_zeta { namespace mailbox {

    auto message::command() const noexcept -> message_id {
        return command_;
    }

    message::operator bool() {
        return command_ != 0 || bool(sender_) || !body_.empty();
    }

    message::message(std::pmr::memory_resource* resource, address_t sender, message_id name, intrusive_ptr<actor_zeta::detail::future_state_base> result_slot)
        : sender_(sender.get())
        , command_(std::move(name))
        , body_(resource)
        , result_slot_(std::move(result_slot)) {}

    message::message(std::pmr::memory_resource* resource, address_t sender, message_id name, actor_zeta::detail::rtt&& body, intrusive_ptr<actor_zeta::detail::future_state_base> result_slot)
        : sender_(sender.get())
        , command_(std::move(name))
        , body_(std::allocator_arg, resource, std::move(body))
        , result_slot_(std::move(result_slot)) {}

    message::message(message&& other) noexcept
        : sender_(std::move(other.sender_))
        , command_(std::move(other.command_))
        , body_(std::move(other.body_))
        , result_slot_(std::move(other.result_slot_)) {
    }

    message::message(std::allocator_arg_t, std::pmr::memory_resource* resource, message&& other) noexcept
        : sender_(std::move(other.sender_))
        , command_(std::move(other.command_))
        , body_(std::allocator_arg, resource, std::move(other.body_))
        , result_slot_(std::move(other.result_slot_)) {
    }

    message& message::operator=(message&& other) noexcept {
        if (this == &other)
            return *this;

        sender_ = std::move(other.sender_);
        command_ = std::move(other.command_);
        body_ = std::move(other.body_);
        result_slot_ = std::move(other.result_slot_);

        return *this;
    }

    message::message(std::pmr::memory_resource* resource)
        : singly_linked(nullptr)
        , prev(nullptr)
        , sender_(address_t::empty_address().get())
        , body_(resource)
        , result_slot_(nullptr) {}

    message::~message() noexcept {}

    void message::swap(message& other) noexcept {
        using std::swap;
        swap(sender_, other.sender_);
        swap(command_, other.command_);
        swap(body_, other.body_);
        swap(result_slot_, other.result_slot_);
    }

    bool message::is_high_priority() const {
        return message_priority(command_) == detail::high_message_priority;
    }

    auto message::body() -> actor_zeta::detail::rtt& {
        assert(!body_.empty());
        return body_;
    }

    auto message::sender() const noexcept -> actor::address_t {
        if (sender_ == nullptr) {
            return actor::address_t::empty_address();
        }
        return actor::address_t(sender_);
    }

}} // namespace actor_zeta::mailbox
