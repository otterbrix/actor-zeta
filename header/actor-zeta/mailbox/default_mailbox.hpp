#pragma once

#include <actor-zeta/detail/queue/enqueue_result.hpp>
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

    template<class T>
    class mailbox_t final : protected T {
    public:
        mailbox_t() = default;

        ~mailbox_t() = default;

        actor_zeta::detail::enqueue_result push_back(mailbox::message_ptr ptr) {
            return self()->push_back_impl(std::move(ptr));
        }
        void push_front(mailbox::message* ptr) {
            return self()->push_front_impl(ptr);
        }
        mailbox::message_ptr pop_front() {
            return self()->pop_front_impl();
        }
        bool closed() const noexcept {
            return self()->closed_impl();
        }
        bool blocked() const noexcept {
            return self()->blocked_impl();
        }
        bool try_block() {
            return self()->try_block_impl();
        }
        bool try_unblock() {
            return self()->try_unblock_impl();
        }
        size_t close() {
            return self()->close_impl();
        }
        size_t size() {
            return self()->size_impl();
        }
        bool empty() {
            return size() == 0;
        }

    private:
        auto self() noexcept -> T* {
            return static_cast<T*>(this);
        }

        auto self() const noexcept -> const T* {
            return static_cast<const T*>(this);
        }
    };

    using default_mailbox = actor_zeta::mailbox::mailbox_t<default_mailbox_impl>;

}} // namespace actor_zeta::mailbox