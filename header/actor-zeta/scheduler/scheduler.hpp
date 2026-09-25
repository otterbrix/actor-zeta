#pragma once

#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <memory_resource>
#include <mutex>

#include <actor-zeta/detail/memory.hpp>
#include <actor-zeta/scheduler/job_ptr.hpp>
#include <actor-zeta/scheduler/worker.hpp>

namespace actor_zeta { namespace scheduler {

    // Takes its memory from `resource` -- the job queue and the workers -- never from the global
    // operator new; only std::thread allocates on its own, in start().
    template<class Policy>
    class scheduler_t {
    public:
        using policy_data = typename Policy::coordinator_data;
        using worker_type = worker<Policy>;

        scheduler_t(std::pmr::memory_resource* resource, size_t num_worker_threads, size_t max_throughput_param)
            : resource_(resource)
            , max_throughput_(max_throughput_param)
            , num_workers_(num_worker_threads)
            , data_(resource) {
            assert(resource && "scheduler_t: resource must not be null");
        }

        // Frees the workers start() made. A plain array, not a std::pmr::vector: libstdc++ 11 destroys
        // a vector's elements through the deprecated polymorphic_allocator::destroy, which Clang
        // reports and -Werror fails. Destroying a started scheduler without stop() still terminates:
        // ~std::thread on a joinable worker.
        ~scheduler_t() {
            if (workers_) {
                for (size_t i = 0; i < num_workers_; ++i) {
                    pmr::deallocate_ptr(resource_, workers_[i]);
                }
                resource_->deallocate(workers_, num_workers_ * sizeof(worker_type*), alignof(worker_type*));
            }
        }

        inline size_t max_throughput() const {
            return max_throughput_;
        }

        inline size_t num_workers() const {
            return num_workers_;
        }

        worker_type* worker_by_id(size_t x) {
            return workers_[x];
        }

        policy_data& data() {
            return data_;
        }

        void start() {
            typename worker_type::policy_data init{this};
            workers_ = static_cast<worker_type**>(
                resource_->allocate(num_workers_ * sizeof(worker_type*), alignof(worker_type*)));
            for (size_t i = 0; i < num_workers_; ++i) {
                workers_[i] = pmr::allocate_ptr<worker_type>(resource_, i, this, init, max_throughput_);
            }

            for (size_t i = 0; i < num_workers_; ++i) {
                workers_[i]->start();
            }
        }

        void stop() {
            // One shutdown job per worker, one at a time: whichever worker takes it finishes what
            // it was running and leaves its loop.
            struct shutdown_helper {
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

                std::mutex mtx;
                std::condition_variable cv;
                size_t completed_count = 0;
            };
            shutdown_helper sh;
            for (size_t i = 0; i < num_workers_; ++i) {
                workers_[i]->external_enqueue(&sh);
                sh.wait_for_completion();
            }

            for (size_t i = 0; i < num_workers_; ++i) {
                workers_[i]->get_thread().join();
            }

            auto clear_job = [](job_ptr) {};
            for (size_t i = 0; i < num_workers_; ++i) {
                policy_.foreach_resumable(workers_[i], clear_job);
            }
            policy_.foreach_central_resumable(this, clear_job);
        }

        template<typename T>
        void enqueue(T* actor) {
            policy_.central_enqueue(this, actor);
        }

    private:
        std::pmr::memory_resource* resource_;
        size_t max_throughput_;
        size_t num_workers_;
        worker_type** workers_ = nullptr; // num_workers_ of them, from resource_, made by start()
        policy_data data_;
        Policy policy_;
    };

}} // namespace actor_zeta::scheduler
