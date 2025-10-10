#pragma once

#include "actor-zeta/detail/queue/enqueue_result.hpp"
#include "actor-zeta/detail/queue/lifo_inbox.hpp"
#include "actor-zeta/detail/queue/linked_list.hpp"

namespace actor_zeta { namespace mailbox {

    class default_mailbox_impl {
    public:
        default_mailbox_impl() = default;
        ~default_mailbox_impl();
        default_mailbox_impl(const default_mailbox_impl&) = delete;
        default_mailbox_impl& operator=(const default_mailbox_impl&) = delete;

        actor_zeta::detail::enqueue_result push_back_impl(message_ptr);
        void push_front_impl(message_ptr);
        message_ptr pop_front_impl();
        bool closed_impl() const noexcept;
        bool blocked_impl() const noexcept;
        bool try_block_impl();
        bool try_unblock_impl();
        size_t close_impl();
        size_t size_impl();
        message* peek_impl(message_id id);

    private:
        size_t cached() const noexcept;
        bool fetch_more();
        actor_zeta::detail::linked_list<message> urgent_queue_;
        actor_zeta::detail::linked_list<message> normal_queue_;
        alignas(CACHE_LINE_SIZE) actor_zeta::detail::lifo_inbox<message> inbox_;
    };
}} // namespace actor_zeta::mailbox