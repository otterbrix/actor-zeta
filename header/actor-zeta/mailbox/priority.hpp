#pragma once

#include <cstdint>
#include <string>
#include <type_traits>

namespace actor_zeta { namespace mailbox {

    enum class priority {
        high = 0,
        normal = 1,
    };

    using high_priority_constant = std::integral_constant<priority, priority::high>;

    using normal_priority_constant = std::integral_constant<priority, priority::normal>;

}} // namespace actor_zeta::mailbox
