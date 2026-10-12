#pragma once

#include <cstdint>

namespace actor_zeta {

    template<typename T>
    class unique_future;

    template<typename T>
    class promise;

    namespace detail {

        enum class enqueue_result : uint8_t;

        class rtt;

        template<typename T>
        struct shared_state;

    } // namespace detail

} // namespace actor_zeta