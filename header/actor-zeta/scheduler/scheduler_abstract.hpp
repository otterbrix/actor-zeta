#pragma once

#include <cstddef>

#include "forwards.hpp"
#include "job_ptr.hpp"

namespace actor_zeta { namespace scheduler {

    /// @brief Cleanup and release a job
    /// @param job Job to cleanup and release
    void cleanup_and_release(job_ptr job);

}} // namespace actor_zeta::scheduler
