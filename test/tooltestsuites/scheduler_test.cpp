// clang-format off
#include <actor-zeta/actor/address.hpp>
#include "scheduler_test.hpp"
// clang-format on

#include "actor-zeta/scheduler/job_ptr.hpp"

#include <limits>

namespace actor_zeta { namespace test {

    scheduler_test_t::scheduler_test_t(std::size_t num_worker_threads, std::size_t max_throughput)
        : max_throughput_(max_throughput)
        , num_workers_(num_worker_threads) {
    }

    void scheduler_test_t::start() {}

    void scheduler_test_t::stop() {
        // "queue non-empty" is not a termination condition: a behavior suspended on a
        // pending co_await legitimately returns `resume` with zero messages handled,
        // forever. Drain until a full sweep of the queue makes no progress.
        for (;;) {
            const size_t n = jobs.size();
            if (n == 0) {
                return;
            }
            size_t progressed = 0;
            for (size_t i = 0; i < n && run_once(); ++i) {
                progressed += last_messages_processed_;
            }
            if (progressed == 0) {
                return;
            }
        }
    }

    void scheduler_test_t::enqueue(scheduler::job_ptr job) {
        jobs.push_back(job);
    }

    bool scheduler_test_t::run_once() {
        if (jobs.empty()) {
            return false;
        }
        auto job = jobs.front();
        jobs.pop_front();
        auto info = job.resume(1);
        last_messages_processed_ = info.messages_processed;
        switch (info.result) {
            case scheduler::resume_result::resume:
                // FIFO, matching work_sharing::enqueue. push_front would let one
                // spinning job monopolise the deque and starve every other actor.
                jobs.push_back(job);
                break;
            case scheduler::resume_result::done:
            case scheduler::resume_result::awaiting:
                break;
            case scheduler::resume_result::shutdown:
                break;
        }
        return true;
    }

    size_t scheduler_test_t::run(size_t max_count) {
        size_t res = 0;
        while (res < max_count && run_once()) {
            ++res;
        }
        return res;
    }


}} // namespace actor_zeta::test
