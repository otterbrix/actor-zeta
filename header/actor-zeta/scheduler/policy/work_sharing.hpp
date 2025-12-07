#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>

#include <actor-zeta/detail/queue/linked_list.hpp>
#include <actor-zeta/scheduler/forwards.hpp>
#include <actor-zeta/scheduler/job_ptr.hpp>
#include <actor-zeta/scheduler/policy/unprofiled.hpp>

namespace actor_zeta { namespace scheduler {

    class work_sharing : public unprofiled {
    public:
        using queue_type = actor_zeta::detail::linked_list<job_ptr>;

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

        // Single enqueue - accepts node (new or reused)
        template<class Coordinator>
        bool enqueue(Coordinator* self, std::unique_ptr<job_ptr> node) {
            std::unique_lock<std::mutex> guard(cast(self).lock);
            cast(self).queue.push_back(node.release());
            guard.unlock();
            cast(self).cv.notify_one();
            return true;
        }

        template<class Coordinator, class Resumable>
        void central_enqueue(Coordinator* self, Resumable* resumable) {
            auto node = std::make_unique<job_ptr>(resumable, &scheduler::detail::resume_impl<Resumable>);
            enqueue(self, std::move(node));
        }

        template<class Worker, class Resumable>
        void external_enqueue(Worker* self, Resumable* resumable) {
            auto node = std::make_unique<job_ptr>(resumable, &scheduler::detail::resume_impl<Resumable>);
            enqueue(self->parent(), std::move(node));
        }

        template<class Worker, class Resumable>
        void internal_enqueue(Worker* self, Resumable* resumable) {
            auto node = std::make_unique<job_ptr>(resumable, &scheduler::detail::resume_impl<Resumable>);
            enqueue(self->parent(), std::move(node));
        }

        template<class Worker>
        void resume_job_later(Worker* self, std::unique_ptr<job_ptr> node) {
            enqueue(self->parent(), std::move(node));
        }

        template<class Worker>
        std::unique_ptr<job_ptr> dequeue(Worker* self) {
            auto& parent_data = cast(self->parent());
            std::unique_lock<std::mutex> guard(parent_data.lock);
            parent_data.cv.wait(guard, [&] { return !parent_data.queue.empty(); });
            return parent_data.queue.pop_front();
        }

        template<class Worker, class UnaryFunction>
        void foreach_resumable(Worker*, UnaryFunction) {}

        template<class Coordinator, class UnaryFunction>
        void foreach_central_resumable(Coordinator* self, UnaryFunction f) {
            auto& queue = cast(self).queue;
            auto next = [&]() -> job_ptr {
                if (queue.empty()) {
                    return job_ptr();
                }
                auto node_ptr = queue.pop_front();
                return *node_ptr;
            };
            std::unique_lock<std::mutex> guard(cast(self).lock);
            for (auto job = next(); job; job = next()) {
                f(job);
            }
        }
    };

}} // namespace actor_zeta::scheduler
