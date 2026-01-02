#pragma once

#include <cstdint>
#include <memory>

namespace actor_zeta { namespace mailbox {

    // Priority enum
    enum class priority;

    // Message ID type
    using message_id = uint64_t;

    // Message class
    class message;

    /// Deleter for message - definition in message.hpp
    struct message_deleter {
        void operator()(message* p) const noexcept;
    };

    using message_ptr = std::unique_ptr<message, message_deleter>;

    // Mailbox types
    class default_mailbox_impl;

    template<class Impl>
    class mailbox_t;

    using default_mailbox = mailbox_t<default_mailbox_impl>;

}} // namespace actor_zeta::mailbox