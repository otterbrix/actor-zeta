#pragma once

#include <cstddef>
#include <limits>

#include <deque>

#include "actor-zeta/scheduler/job_ptr.hpp"

namespace actor_zeta { namespace test {

    class scheduler_test_t final {
    public:
        scheduler_test_t(std::size_t num_worker_threads, std::size_t max_throughput);

        std::deque<scheduler::job_ptr> jobs;

        bool run_once();
        size_t run(size_t max_count = std::numeric_limits<size_t>::max());

        void start();
        void stop();
        void enqueue(scheduler::job_ptr job);

        template<typename T>
        void enqueue(T* actor) {
            enqueue(scheduler::job_ptr(actor, &scheduler::detail::resume_impl<T>));
        }

        inline size_t max_throughput() const {
            return max_throughput_;
        }

        inline size_t num_workers() const {
            return num_workers_;
        }

    private:
        size_t max_throughput_;
        size_t num_workers_;
    };

}} // namespace actor_zeta::test
