#pragma once

#include <memory>
#include <actor-zeta/scheduler/scheduler.hpp>
#include <actor-zeta/scheduler/policy/work_sharing.hpp>
#include <actor-zeta/detail/memory_resource.hpp>

namespace actor_zeta { namespace scheduler {

    using sharing_scheduler = scheduler_t<work_sharing>;

    inline scheduler_abstract_t* make_sharing_scheduler(
        actor_zeta::pmr::memory_resource* /* resource */,
        std::size_t num_worker_threads,
        std::size_t max_throughput
    ) {
        return new sharing_scheduler(num_worker_threads, max_throughput);
    }

}} // namespace actor_zeta::scheduler