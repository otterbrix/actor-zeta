#pragma once

#include <cstdint>

namespace actor_zeta::detail::state_flags {

    // Bit flags for shared_state synchronization (Last-One-Out pattern)

    inline constexpr std::uint8_t empty            = 0b0000'0000;  // initial state
    inline constexpr std::uint8_t value_set        = 0b0000'0001;  // value has been written
    inline constexpr std::uint8_t error_set        = 0b0000'0010;  // error has been written
    inline constexpr std::uint8_t consumed         = 0b0000'0100;  // result has been consumed (debug)
    inline constexpr std::uint8_t promise_released  = 0b0000'1000;  // promise side released
    inline constexpr std::uint8_t future_released   = 0b0001'0000;  // future side released
    inline constexpr std::uint8_t promise_finalizing = 0b0010'0000;  // producer in final_suspend (prevents race)

    // Composite masks
    inline constexpr std::uint8_t result_set    = value_set | error_set;
    inline constexpr std::uint8_t both_released = promise_released | future_released;

} // namespace actor_zeta::detail::state_flags
