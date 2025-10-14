#pragma once

#include <actor-zeta/scheduler/scheduler.hpp>
#include <actor-zeta/scheduler/policy/work_sharing.hpp>

namespace actor_zeta { namespace scheduler {

    using sharing_scheduler = scheduler_t<work_sharing>;

}} // namespace actor_zeta::scheduler