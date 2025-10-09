// clang-format off
#include <actor-zeta/base/address.hpp>
#include "scheduler_test.hpp"
// clang-format on

#include "actor-zeta/scheduler/resumable.hpp"

#include <limits>

namespace actor_zeta { namespace test {

    namespace {

        class dummy_worker {
        public:
            dummy_worker(scheduler_test_t* parent)
                : parent_(parent) {
            }

            void execute_later(scheduler::resumable_t* ptr) {
                parent_->jobs.push_back(ptr);
            }

        private:
            scheduler_test_t* parent_;
        };

    } // namespace

    scheduler_test_t::scheduler_test_t(std::size_t num_worker_threads, std::size_t max_throughput)
        : scheduler_abstract_t(num_worker_threads, max_throughput) {
    }

    void scheduler_test_t::start() {}

    void scheduler_test_t::stop() {
        while (run() > 0) {}
    }

    void scheduler_test_t::enqueue(scheduler::resumable_t* ptr) {
        jobs.push_back(ptr);
    }

    bool scheduler_test_t::run_once() {
        if (jobs.empty()) {
            return false;
        }
        auto job = jobs.front();
        jobs.pop_front();
        dummy_worker worker{this};
        switch (job->resume(this, 1)) {
            case scheduler::resume_result::resume:
                jobs.push_front(job);
                break;
            case scheduler::resume_result::done:
            case scheduler::resume_result::awaiting:
                intrusive_ptr_release(job);
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
