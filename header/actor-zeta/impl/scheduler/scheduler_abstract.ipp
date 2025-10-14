#pragma once

#include <actor-zeta/scheduler/scheduler_abstract.hpp>
#include <actor-zeta/scheduler/job_ptr.hpp>

namespace actor_zeta { namespace scheduler {

    void cleanup_and_release(job_ptr job) {
        job.release();
    }

}} // namespace actor_zeta::scheduler