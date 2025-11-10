#pragma once

#include <actor-zeta/scheduler/job_ptr.hpp>

namespace actor_zeta { namespace scheduler {

    class unprofiled {
    public:
        virtual ~unprofiled();

        template<class Worker>
        void before_shutdown(Worker*) {}

        template<class Worker>
        void before_resume(Worker*, job_ptr) {}

        template<class Worker>
        void after_resume(Worker*, job_ptr) {}

        template<class Worker>
        void after_completion(Worker*, job_ptr) {}

    protected:
        template<class WorkerOrCoordinator>
        static auto cast(WorkerOrCoordinator* self) -> decltype(self->data()) {
            return self->data();
        }
    };

}} // namespace actor_zeta::scheduler
