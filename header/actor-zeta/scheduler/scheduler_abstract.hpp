#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>

#include "forwards.hpp"

namespace actor_zeta { namespace scheduler {

    class scheduler_abstract_t {
    public:
        scheduler_abstract_t(
            std::size_t num_worker_threads,
            std::size_t max_throughput);

        virtual ~scheduler_abstract_t() = default;
        virtual void enqueue(resumable*) = 0;

        inline size_t max_throughput() const {
            return max_throughput_;
        }

        inline size_t num_workers() const {
            return num_workers_;
        }

        virtual void start() = 0;
        virtual void stop() = 0;

    protected:
        std::atomic<size_t> next_worker_;
        size_t max_throughput_;
        size_t num_workers_;
    };

    void cleanup_and_release(resumable*);

}} // namespace actor_zeta::scheduler
