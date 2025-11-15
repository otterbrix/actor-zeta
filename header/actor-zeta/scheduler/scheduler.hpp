#pragma once

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <limits>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include <actor-zeta/detail/ref_counted.hpp>
#include <actor-zeta/scheduler/worker.hpp>
#include <actor-zeta/scheduler/job_ptr.hpp>

namespace actor_zeta { namespace scheduler {

    template<class Policy>
    class scheduler_t {
    public:
        using policy_data = typename Policy::coordinator_data;
        using worker_type = worker<Policy>;

        scheduler_t(size_t num_worker_threads, size_t max_throughput_param)
            : next_worker_(0)
            , max_throughput_(max_throughput_param)
            , num_workers_(num_worker_threads)
            , data_(this) {
        }

        inline size_t max_throughput() const {
            return max_throughput_;
        }

        inline size_t num_workers() const {
            return num_workers_;
        }

        worker_type* worker_by_id(size_t x) {
            return workers_[x].get();
        }

        policy_data& data() {
            return data_;
        }

        void start() {
            typename worker_type::policy_data init{this};
            auto num = num_workers();
            workers_.reserve(num);

            for (size_t i = 0; i < num; ++i) {
                workers_.emplace_back(new worker_type(i, this, init, max_throughput_));
            }

            for (auto& w : workers_) {
                w->start();
            }

        }

        void stop() {
            class shutdown_helper : public ref_counted {
            public:
                resume_info resume(size_t) {
                    std::unique_lock<std::mutex> guard(mtx);
                    ++completed_count;
                    cv.notify_all();
                    return resume_info(resume_result::shutdown, 0);
                }

                void wait_for_completion() {
                    std::unique_lock<std::mutex> guard(mtx);
                    cv.wait(guard, [this] { return completed_count > 0; });
                    --completed_count;
                }

                shutdown_helper(): completed_count(0) {}

                std::mutex mtx;
                std::condition_variable cv;
                std::atomic<size_t> completed_count;
            };
            // Use a set to keep track of remaining workers.
            shutdown_helper sh;
            std::set<worker_type*> alive_workers;
            auto num = num_workers();
            for (size_t i = 0; i < num; ++i) {
                alive_workers.insert(worker_by_id(i));
                sh.ref(); // Make sure reference count is high enough.
            }
            while (!alive_workers.empty()) {
                auto it = alive_workers.begin();
                (*it)->external_enqueue(&sh);
                sh.wait_for_completion();
                alive_workers.erase(it);
            }

            for (auto& w : workers_) {
                w->get_thread().join(); /// wait until all workers finish working
            }

            /// Clear remaining job_ptr from queues (actor lifetime managed by unique_ptr)
            auto clear_job = [](job_ptr) { /* NO-OP: job_ptr does not own actor */ };
            for (auto& w : workers_) {
                policy_.foreach_resumable(w.get(), clear_job);
            }
            policy_.foreach_central_resumable(this, clear_job);
        }

        /// @brief Enqueue an actor for execution
        /// @tparam T Actor type with resume(size_t) method
        /// @param actor Pointer to actor to schedule
        template<typename T>
        void enqueue(T* actor) {
            policy_.central_enqueue(this, actor);
        }

    private:
        std::atomic<size_t> next_worker_;
        size_t max_throughput_;
        size_t num_workers_;
        std::vector<std::unique_ptr<worker_type>> workers_;
        policy_data data_;
        Policy policy_;
    };

}} // namespace actor_zeta::scheduler