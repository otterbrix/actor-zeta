#pragma once

namespace actor_zeta { namespace scheduler {
    class work_sharing;
    struct resumable;
    using resumable_t = resumable;
    class scheduler_abstract_t;
    template<class Policy>
    class scheduler_t ;
}} // namespace actor_zeta::scheduler
