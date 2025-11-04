#pragma once

#include <condition_variable>
#include <cstddef>
#include <list>
#include <mutex>

#include <actor-zeta/scheduler/forwards.hpp>
#include <actor-zeta/scheduler/policy/unprofiled.hpp>
#include <actor-zeta/scheduler/job_ptr.hpp>

namespace actor_zeta { namespace scheduler {

    class work_sharing : public unprofiled {
    public:
        using queue_type = std::list<job_ptr>;

        ~work_sharing() override;

        struct coordinator_data {
            template<typename Scheduler>
            explicit coordinator_data(Scheduler*) {}
            queue_type queue;
            std::mutex lock;
            std::condition_variable cv;
        };

        struct worker_data {
            template<typename Scheduler>
            explicit worker_data(Scheduler*) {}
        };

        template<class Coordinator>
        bool enqueue(Coordinator* self, job_ptr job) {
            queue_type l;
            l.push_back(job);
            std::unique_lock<std::mutex> guard(cast(self).lock);
            cast(self).queue.splice(cast(self).queue.end(), l);
            cast(self).cv.notify_one();
            return true;
        }

        template<class Coordinator>
        void central_enqueue(Coordinator* self, job_ptr job) {
            enqueue(self, job);
        }

        template<class Worker>
        void external_enqueue(Worker* self, job_ptr job) {
            enqueue(self->parent(), job);
        }

        template<class Worker>
        void internal_enqueue(Worker* self, job_ptr job) {
            enqueue(self->parent(), job);
        }

        template<class Worker>
        void resume_job_later(Worker* self, job_ptr job) {
            enqueue(self->parent(), job);
        }

        template<class Worker>
        job_ptr dequeue(Worker* self) {
            auto& parent_data = cast(self->parent());
            std::unique_lock<std::mutex> guard(parent_data.lock);
            parent_data.cv.wait(guard, [&] { return !parent_data.queue.empty(); });
            job_ptr job = parent_data.queue.front();
            parent_data.queue.pop_front();
            return job;
        }

        template<class Worker, class UnaryFunction>
        void foreach_resumable(Worker*, UnaryFunction) {}

        template<class Coordinator, class UnaryFunction>
        void foreach_central_resumable(Coordinator* self, UnaryFunction f) {
            auto& queue = cast(self).queue;
            auto next = [&]() -> job_ptr {
                if (queue.empty()) {
                    return job_ptr{nullptr, nullptr};
                }
                auto front = queue.front();
                queue.pop_front();
                return front;
            };
            std::unique_lock<std::mutex> guard(cast(self).lock);
            for (auto job = next(); job; job = next()) {
                f(job);
            }
        }
    };

}} // namespace actor_zeta::scheduler
