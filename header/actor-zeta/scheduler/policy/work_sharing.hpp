#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory_resource>
#include <mutex>
#include <new>

#include <actor-zeta/scheduler/forwards.hpp>
#include <actor-zeta/scheduler/job_ptr.hpp>
#include <actor-zeta/scheduler/policy/unprofiled.hpp>

namespace actor_zeta { namespace scheduler {

    class work_sharing : public unprofiled {
    public:
        ~work_sharing() override;

        // One FIFO for every worker: a ring of jobs -- plain values -- in memory from the
        // scheduler's resource. It grows by doubling, under the queue's lock, and never shrinks:
        // once warmed up, enqueue allocates nothing.
        struct coordinator_data {
            explicit coordinator_data(std::pmr::memory_resource* res)
                : resource(res) {}

            ~coordinator_data() {
                if (jobs) {
                    resource->deallocate(jobs, capacity * sizeof(job_ptr), alignof(job_ptr));
                }
            }

            coordinator_data(const coordinator_data&) = delete;
            coordinator_data& operator=(const coordinator_data&) = delete;

            // Both under `lock`.
            void push_back(job_ptr job) {
                if (size == capacity) {
                    grow();
                }
                new (jobs + (head + size) % capacity) job_ptr(job);
                ++size;
            }

            job_ptr pop_front() {
                const job_ptr job = jobs[head];
                head = (head + 1) % capacity;
                --size;
                return job;
            }

            std::pmr::memory_resource* resource;
            job_ptr* jobs = nullptr;
            std::size_t capacity = 0;
            std::size_t head = 0;
            std::size_t size = 0;
            std::mutex lock;
            std::condition_variable cv;

        private:
            void grow() {
                const std::size_t bigger = capacity == 0 ? 64 : capacity * 2;
                auto* fresh = static_cast<job_ptr*>(resource->allocate(bigger * sizeof(job_ptr), alignof(job_ptr)));
                for (std::size_t i = 0; i < size; ++i) {
                    new (fresh + i) job_ptr(jobs[(head + i) % capacity]);
                }
                if (jobs) {
                    resource->deallocate(jobs, capacity * sizeof(job_ptr), alignof(job_ptr));
                }
                jobs = fresh;
                capacity = bigger;
                head = 0;
            }
        };

        struct worker_data {
            template<typename Scheduler>
            explicit worker_data(Scheduler*) {}
        };

        template<class Coordinator>
        void enqueue(Coordinator* self, job_ptr job) {
            auto& data = cast(self);
            std::unique_lock<std::mutex> guard(data.lock);
            data.push_back(job);
            guard.unlock();
            data.cv.notify_one();
        }

        template<class Coordinator, class Resumable>
        void central_enqueue(Coordinator* self, Resumable* resumable) {
            enqueue(self, job_ptr(resumable, &scheduler::detail::resume_impl<Resumable>));
        }

        template<class Worker, class Resumable>
        void external_enqueue(Worker* self, Resumable* resumable) {
            enqueue(self->parent(), job_ptr(resumable, &scheduler::detail::resume_impl<Resumable>));
        }

        template<class Worker>
        void resume_job_later(Worker* self, job_ptr job) {
            enqueue(self->parent(), job);
        }

        template<class Worker>
        job_ptr dequeue(Worker* self) {
            auto& data = cast(self->parent());
            std::unique_lock<std::mutex> guard(data.lock);
            data.cv.wait(guard, [&] { return data.size != 0; });
            return data.pop_front();
        }

        template<class Worker, class UnaryFunction>
        void foreach_resumable(Worker*, UnaryFunction) {}

        template<class Coordinator, class UnaryFunction>
        void foreach_central_resumable(Coordinator* self, UnaryFunction f) {
            auto& data = cast(self);
            std::unique_lock<std::mutex> guard(data.lock);
            while (data.size != 0) {
                f(data.pop_front());
            }
        }
    };

}} // namespace actor_zeta::scheduler
