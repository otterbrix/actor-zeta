#pragma once

#include <cstddef>
#include <limits>

#include <deque>

#include "actor-zeta/scheduler/scheduler.hpp"

namespace actor_zeta { namespace test {

    class scheduler_test_t final : public scheduler::scheduler_t {
    public:
        scheduler_test_t(std::size_t num_worker_threads, std::size_t max_throughput);

        std::deque<scheduler::resumable_t*> jobs;

        bool run_once();
        size_t run(size_t max_count = std::numeric_limits<size_t>::max());

    ///protected:
        void start() override;
        void stop() override;
        void schedule(scheduler::resumable_t* ptr) override;

    private:
    };

}} // namespace actor_zeta::test
