#pragma once

#include <memory>

#include <actor-zeta/scheduler/sharing_scheduler.hpp>

namespace actor_zeta {

    using scheduler::sharing_scheduler;
    using scheduler_ptr = std::unique_ptr<sharing_scheduler>;
    using scheduler_raw = sharing_scheduler*;

} // namespace actor_zeta