#pragma once
#include <memory>

namespace actor_zeta { namespace mailbox {
    class message;
    struct message_deleter;
    using message_ptr = std::unique_ptr<message, message_deleter>;
}}