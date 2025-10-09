#pragma once

#include <actor-zeta/mailbox/default_mailbox.hpp>

namespace actor_zeta { namespace mailbox {

    actor_zeta::detail::enqueue_result default_mailbox_impl::push_back_impl(message_ptr ptr) {
        return inbox_.push_front(ptr.release());
    }

    void default_mailbox_impl::push_front_impl(message_ptr ptr) {
        if (ptr->is_high_priority()) {
            urgent_queue_.push_front(ptr.release());
        } else {
            normal_queue_.push_front(ptr.release());
        }
    }

    message_ptr default_mailbox_impl::pop_front_impl() {
        for (;;) {
            if (auto result = urgent_queue_.pop_front()) {
                return result;
            }
            if (auto result = normal_queue_.pop_front()) {
                return result;
            }
            if (!fetch_more()) {
                return nullptr;
            }
        }
    }

    bool default_mailbox_impl::closed_impl() const noexcept {
        return inbox_.closed();
    }

    bool default_mailbox_impl::blocked_impl() const noexcept {
        return inbox_.blocked();
    }

    bool default_mailbox_impl::try_block_impl() {
        return cached() == 0 && inbox_.try_block();
    }

    bool default_mailbox_impl::try_unblock_impl() {
        return inbox_.try_unblock();
    }

    size_t default_mailbox_impl::close_impl() {
        size_t result = 0;
        auto bounce_and_count = [&result](message* ptr) {
            delete ptr;
            ++result;
        };
        urgent_queue_.drain(bounce_and_count);
        normal_queue_.drain(bounce_and_count);
        inbox_.close(bounce_and_count);
        return result;
    }

    size_t default_mailbox_impl::size_impl() {
        fetch_more();
        return cached();
    }

    size_t default_mailbox_impl::cached() const noexcept {
        return urgent_queue_.size() + normal_queue_.size();
    }

    bool default_mailbox_impl::fetch_more() {
        using node_type = actor_zeta::detail::singly_linked<message>;
        auto promote = [](node_type* ptr) {
            return static_cast<message*>(ptr);
        };
        auto* head = static_cast<node_type*>(inbox_.take_head());
        if (head == nullptr)
            return false;
        auto urgent_insertion_point = urgent_queue_.before_end();
        auto normal_insertion_point = normal_queue_.before_end();
        do {
            auto next = head->next;
            auto phead = promote(head);
            if (phead->is_high_priority())
                urgent_queue_.insert_after(urgent_insertion_point, phead);
            else
                normal_queue_.insert_after(normal_insertion_point, phead);
            head = next;
        } while (head != nullptr);
        return true;
    }
}} // namespace actor_zeta::mailbox