#pragma once

#include <actor-zeta/scheduler/resumable.hpp>
#include <actor-zeta/scheduler/scheduler_abstract.hpp>

namespace actor_zeta { namespace scheduler {

    class unprofiled {
    public:
        virtual ~unprofiled();

        template<class Worker>
        void before_shutdown(Worker*) {}

        template<class Worker>
        void before_resume(Worker*, resumable*) {}

        template<class Worker>
        void after_resume(Worker*, resumable*) {}

        template<class Worker>
        void after_completion(Worker*, resumable*) {}

    protected:
        template<class WorkerOrCoordinator>
        static auto cast(WorkerOrCoordinator* self) -> decltype(self->data()) {
            return self->data();
        }
    };

}} // namespace actor_zeta::scheduler
