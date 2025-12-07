#pragma once

#include <cstdint>

namespace actor_zeta { namespace detail {

    enum class enqueue_result : uint8_t {
        success,
        unblocked_reader,
        queue_closed
    };

}} // namespace actor_zeta::detail
