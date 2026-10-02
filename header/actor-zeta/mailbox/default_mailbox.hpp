#pragma once

#include <actor-zeta/detail/queue/lifo_inbox.hpp>
#include <actor-zeta/detail/queue/linked_list.hpp>
#include <actor-zeta/mailbox/message.hpp>

namespace actor_zeta { namespace mailbox {

    class default_mailbox_impl {
    public:
        default_mailbox_impl() = default;
        ~default_mailbox_impl();
        default_mailbox_impl(const default_mailbox_impl&) = delete;
        default_mailbox_impl& operator=(const default_mailbox_impl&) = delete;

        actor_zeta::detail::enqueue_result push_back_impl(message_ptr);
        message_ptr pop_front_impl();
        bool try_block_impl();
        size_t close_impl();

    private:
        size_t cached() const noexcept;
        bool fetch_more();
        actor_zeta::detail::linked_list<message, message_deleter> urgent_queue_;
        actor_zeta::detail::linked_list<message, message_deleter> normal_queue_;
        alignas(CACHE_LINE_SIZE) actor_zeta::detail::lifo_inbox<message, message_deleter> inbox_;
    };

    template<class T>
    class mailbox_t final : protected T {
    public:
        mailbox_t() = default;

        ~mailbox_t() = default;

        actor_zeta::detail::enqueue_result push_back(mailbox::message_ptr ptr) {
            return self()->push_back_impl(std::move(ptr));
        }
        mailbox::message_ptr pop_front() {
            return self()->pop_front_impl();
        }
        bool try_block() {
            return self()->try_block_impl();
        }
        size_t close() {
            return self()->close_impl();
        }

    private:
        auto self() noexcept -> T* {
            return static_cast<T*>(this);
        }
    };

    using default_mailbox = actor_zeta::mailbox::mailbox_t<default_mailbox_impl>;

}} // namespace actor_zeta::mailbox