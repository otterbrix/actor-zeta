#pragma once

#include <condition_variable>
#include <cstddef>
#include <list>
#include <mutex>

#include <actor-zeta/scheduler/forwards.hpp>
#include <actor-zeta/scheduler/policy/unprofiled.hpp>
#include <actor-zeta/scheduler/resumable.hpp>

namespace actor_zeta { namespace scheduler {

    class work_sharing : public unprofiled {
    public:
        using queue_type = std::list<resumable*>;

        ~work_sharing() override;

        struct coordinator_data {
            explicit coordinator_data(scheduler_abstract_t*) {}
            queue_type queue;
            std::mutex lock;
            std::condition_variable cv;
        };

        struct worker_data {
            explicit worker_data(scheduler_abstract_t*) {}
        };

        template<class Coordinator>
        bool enqueue(Coordinator* self, resumable* job) {
            queue_type l;
            l.push_back(job);
            std::unique_lock<std::mutex> guard(cast(self).lock);
            cast(self).queue.splice(cast(self).queue.end(), l);
            cast(self).cv.notify_one();
            return true;
        }

        template<class Coordinator>
        void central_enqueue(Coordinator* self, resumable* job) {
            enqueue(self, job);
        }

        template<class Worker>
        void external_enqueue(Worker* self, resumable* job) {
            enqueue(self->parent(), job);
        }

        template<class Worker>
        void internal_enqueue(Worker* self, resumable* job) {
            enqueue(self->parent(), job);
        }

        template<class Worker>
        void resume_job_later(Worker* self, resumable* job) {
            enqueue(self->parent(), job);
        }

        template<class Worker>
        resumable* dequeue(Worker* self) {
            auto& parent_data = cast(self->parent());
            std::unique_lock<std::mutex> guard(parent_data.lock);
            // @TODO CRITICAL BUG on condition in cooperative_actor::enqueue_impl: 'flags() != static_cast<int>(state::empty)'
            // infinite waiting on condition_variable
            parent_data.cv.wait(guard, [&] { return !parent_data.queue.empty(); });
            resumable* job = parent_data.queue.front();
            parent_data.queue.pop_front();
            return job;
        }

        template<class Worker, class UnaryFunction>
        void foreach_resumable(Worker*, UnaryFunction) {}

        template<class Coordinator, class UnaryFunction>
        void foreach_central_resumable(Coordinator* self, UnaryFunction f) {
            auto& queue = cast(self).queue;
            auto next = [&]() -> resumable* {
                if (queue.empty()) {
                    return nullptr;
                }
                auto front = queue.front();
                queue.pop_front();
                return front;
            };
            std::unique_lock<std::mutex> guard(cast(self).lock);
            for (auto job = next(); job != nullptr; job = next()) {
                f(job);
            }
        }
    };

}} // namespace actor_zeta::scheduler
