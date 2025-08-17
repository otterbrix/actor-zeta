#pragma once
#include <memory>

namespace actor_zeta { namespace mailbox {
    class message;
    using message_ptr = std::unique_ptr<message>;
}}