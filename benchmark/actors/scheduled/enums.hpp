#pragma once

#include <stdint.h>

enum class command_t : uint64_t {
    add_link,
    add_address,
    start,
    ping,
    pong,
    prepare,
    send,
};

enum class name_t : uint64_t {
    actor_0,
    actor_1,
};
