#pragma once

#include <cstdint>

namespace actor_zeta {

    // Core async types
    template<typename T>
    class unique_future;

    template<typename T>
    class promise;

    template<typename T>
    class generator;

    // Smart pointer
    template<class T>
    class intrusive_ptr;

    namespace detail {

        // Enqueue result
        enum class enqueue_result : uint8_t;

        // Runtime type tuple
        class rtt;

        // shared_state (for unique_future)
        template<typename T>
        struct shared_state;

        // Generator state
        template<typename T>
        class generator_state;

    } // namespace detail

} // namespace actor_zeta
