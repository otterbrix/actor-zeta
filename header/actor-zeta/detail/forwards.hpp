#pragma once

#include <cstdint>

namespace actor_zeta {

    template<typename T>
    class unique_future;

    template<typename T>
    class promise;

    template<typename T>
    class generator;

    template<class T>
    class intrusive_ptr;

    namespace detail {

        enum class enqueue_result : uint8_t;

        class rtt;

        template<typename T>
        struct shared_state;

        template<typename T>
        class generator_state;

    } // namespace detail

} // namespace actor_zeta