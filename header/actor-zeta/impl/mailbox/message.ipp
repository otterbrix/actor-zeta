#pragma once

#include <utility>

#include <actor-zeta/base/address.hpp>
#include <actor-zeta/mailbox/message.hpp>

namespace actor_zeta { namespace mailbox {

    auto message::command() const noexcept -> message_id {
        return command_;
    }

    message::operator bool() {
        return /*!command_ ||*/ bool(sender_) || !body_.empty();
    }

    message::message(actor_zeta::pmr::memory_resource* resource,address_t sender, message_id name)
        : sender_(std::move(sender))
        , command_(std::move(name))
        , body_(resource) {}

    message::message(actor_zeta::pmr::memory_resource* resource,address_t sender, message_id name, actor_zeta::detail::rtt body)
        : sender_(std::move(sender))
        , command_(std::move(name))
        , body_(resource,std::move(body)) {}

    void message::swap(message& other) noexcept {
        using std::swap;
        swap(sender_, other.sender_);
        swap(command_, other.command_);
        swap(body_, other.body_);
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

}} // namespace actor_zeta::base
