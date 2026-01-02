#pragma once

namespace actor_zeta { namespace scheduler {

    // Policy classes
    class work_sharing;
    class unprofiled;

    // Resumable interface
    struct resumable;
    using resumable_t = resumable;

    // Scheduler template
    template<class Policy>
    class scheduler_t;

    // Worker template
    template<class Policy>
    class worker;

    // Resume types
    enum class resume_result;
    struct resume_info;

    // Type aliases
    using sharing_scheduler = scheduler_t<work_sharing>;

}} // namespace actor_zeta::scheduler
