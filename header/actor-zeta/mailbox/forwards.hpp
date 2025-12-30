#pragma once
#include <memory>

namespace actor_zeta { namespace mailbox {
    class message;

    /// Deleter for message - definition in message.hpp
    struct message_deleter {
        void operator()(message* p) const noexcept;
    };

    using message_ptr = std::unique_ptr<message, message_deleter>;
}} // namespace actor_zeta::mailbox