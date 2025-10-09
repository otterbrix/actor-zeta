#pragma once

#include <actor-zeta/scheduler/scheduler_abstract.hpp>
#include <actor-zeta/scheduler/resumable.hpp>

namespace actor_zeta { namespace scheduler {

    scheduler_abstract_t::scheduler_abstract_t(
        std::size_t num_worker_threads,
        std::size_t max_throughput)
        : next_worker_(0)
        , max_throughput_(max_throughput)
        , num_workers_(num_worker_threads) {
    }

    void cleanup_and_release(resumable* ptr) {
        intrusive_ptr_release(ptr);
    }

}} // namespace actor_zeta::scheduler