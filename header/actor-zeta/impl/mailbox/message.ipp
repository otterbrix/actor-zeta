#pragma once

#include <utility>

#include <actor-zeta/actor/address.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/detail/future.hpp>
#include <actor-zeta/detail/shared_state.hpp>

namespace actor_zeta { namespace mailbox {

    auto message::command() const noexcept -> message_id {
        return command_;
    }

    message::operator bool() {
        return command_ != 0 || !body_.empty();
    }

    message::message(std::pmr::memory_resource* resource, message_id name)
        : command_(std::move(name))
        , body_(resource)
        , result_slot_(nullptr)
        , cleanup_fn_(nullptr)
        , ownership_transferred_(false) {}

    message::message(std::pmr::memory_resource* resource, message_id name, actor_zeta::detail::rtt&& body)
        : command_(std::move(name))
        , body_(std::allocator_arg, resource, std::move(body))
        , result_slot_(nullptr)
        , cleanup_fn_(nullptr)
        , ownership_transferred_(false) {}

    message::message(message&& other) noexcept
        : command_(std::move(other.command_))
        , body_(std::move(other.body_))
        , result_slot_(std::exchange(other.result_slot_, nullptr))
        , cleanup_fn_(std::exchange(other.cleanup_fn_, nullptr))
        , ownership_transferred_(std::exchange(other.ownership_transferred_, false)) {
    }

    message::message(std::allocator_arg_t, std::pmr::memory_resource* resource, message&& other) noexcept
        : command_(std::move(other.command_))
        , body_(std::allocator_arg, resource, std::move(other.body_))
        , result_slot_(std::exchange(other.result_slot_, nullptr))
        , cleanup_fn_(std::exchange(other.cleanup_fn_, nullptr))
        , ownership_transferred_(std::exchange(other.ownership_transferred_, false)) {
    }

    message& message::operator=(message&& other) noexcept {
        if (this == &other)
            return *this;

        // Call cleanup on current slot if not transferred
        if (!ownership_transferred_ && cleanup_fn_ && result_slot_) {
            cleanup_fn_(result_slot_);
        }

        command_ = std::move(other.command_);
        body_ = std::move(other.body_);
        result_slot_ = std::exchange(other.result_slot_, nullptr);
        cleanup_fn_ = std::exchange(other.cleanup_fn_, nullptr);
        ownership_transferred_ = std::exchange(other.ownership_transferred_, false);

        return *this;
    }

    message::message(std::pmr::memory_resource* resource)
        : singly_linked(nullptr)
        , prev(nullptr)
        , body_(resource)
        , result_slot_(nullptr)
        , cleanup_fn_(nullptr)
        , ownership_transferred_(false) {}

    message::~message() noexcept {
        // Call cleanup if ownership was not transferred
        if (!ownership_transferred_ && cleanup_fn_ && result_slot_) {
            cleanup_fn_(result_slot_);
        }
    }

    void message::swap(message& other) noexcept {
        using std::swap;
        swap(command_, other.command_);
        swap(body_, other.body_);
        swap(result_slot_, other.result_slot_);
        swap(cleanup_fn_, other.cleanup_fn_);
        swap(ownership_transferred_, other.ownership_transferred_);
    }

    bool message::is_high_priority() const {
        return message_priority(command_) == detail::high_message_priority;
    }

    auto message::body() -> actor_zeta::detail::rtt& {
        assert(!body_.empty());
        return body_;
    }

    // NOTE: get_result_promise<T> is now defined in message.hpp (template must be in header)

}} // namespace actor_zeta::mailbox
