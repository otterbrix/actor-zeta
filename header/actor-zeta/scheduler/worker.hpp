#pragma once

#include <cassert>
#include <cstddef>
#include <thread>

#include "actor-zeta/detail/launch_thread.hpp"
#include "resumable.hpp"

namespace actor_zeta { namespace scheduler {

    template<class Policy>
    class scheduler_t;

    template<class Policy>
    class worker final {
    public:
        using job_ptr = resumable*;
        using scheduler_ptr = scheduler_t<Policy>*;
        using policy_data = typename Policy::worker_data;

        worker(size_t worker_id, scheduler_ptr worker_parent,
               const policy_data& init, size_t throughput)
            : max_throughput_(throughput)
            , id_(worker_id)
            , parent_(worker_parent)
            , data_(init) {}

        void start() {
            assert(this_thread_.get_id() == std::thread::id{}); /// TODO: see implement asio
            this_thread_ = detail::launch_thread("worker", [this] { run(); });
        }

        worker(worker&) = delete;
        worker& operator=(worker&) = delete;
        worker(const worker&) = delete;
        worker& operator=(const worker&) = delete;

        void external_enqueue(job_ptr job) {
            assert(job != nullptr);
            policy_.external_enqueue(this, job);
        }

        void execute_later(job_ptr job) {
            assert(job != nullptr);
            policy_.internal_enqueue(this, job);
        }

        scheduler_ptr parent() {
            return parent_;
        }

        size_t id() const {
            return id_;
        }

        std::thread& get_thread() {
            return this_thread_;
        }

        policy_data& data() {
            return data_;
        }

        size_t max_throughput() {
            return max_throughput_;
        }

    private:
        void run() {
            for (;;) {
                auto job = policy_.dequeue(this);
                assert(job != nullptr);
                policy_.before_resume(this, job);
                auto res = job->resume(parent_, max_throughput_);
                policy_.after_resume(this, job);
                switch (res) {
                    case resume_result::resume: {
                        policy_.resume_job_later(this, job);
                        break;
                    }
                    case resume_result::done: {
                        policy_.after_completion(this, job);
                        intrusive_ptr_release(job);
                        break;
                    }
                    case resume_result::awaiting: {
                        intrusive_ptr_release(job);
                        break;
                    }
                    case resume_result::shutdown: {
                        policy_.after_completion(this, job);
                        policy_.before_shutdown(this);
                        return;
                    }
                }
            }
        }

        size_t max_throughput_;
        std::thread this_thread_;
        size_t id_;
        scheduler_ptr parent_;
        policy_data data_;
        Policy policy_;
    };

}} // namespace actor_zeta::scheduler
