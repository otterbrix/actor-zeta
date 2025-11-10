#pragma once

#include <cassert>
#include <condition_variable>
#include <limits>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include <actor-zeta/detail/ref_counted.hpp>
#include <actor-zeta/scheduler/scheduler_abstract.hpp>
#include <actor-zeta/scheduler/worker.hpp>

namespace actor_zeta { namespace scheduler {

    template<class Policy>
    class scheduler_t : public scheduler_abstract_t {
    public:
        using super = scheduler_abstract_t;
        using policy_data = typename Policy::coordinator_data;
        using worker_type = worker<Policy>;

        scheduler_t(size_t num_worker_threads, size_t max_throughput_param)
            : scheduler_abstract_t(num_worker_threads, max_throughput_param)
            , data_(this) {
        }

        worker_type* worker_by_id(size_t x) {
            return workers_[x].get();
        }

        policy_data& data() {
            return data_;
        }

    protected:
        void start() override {
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

        void stop() override {
            class shutdown_helper
                : public resumable
                , public ref_counted {
            public:
                resume_info resume(scheduler_abstract_t* ptr, size_t) override {
                    assert(ptr != nullptr);
                    std::unique_lock<std::mutex> guard(mtx);
                    last_worker = ptr;
                    cv.notify_all();
                    return resume_info(resume_result::shutdown, 0);
                }

                void intrusive_ptr_add_ref_impl() override {
                    intrusive_ptr_add_ref(this);
                }

                void intrusive_ptr_release_impl() override {
                    intrusive_ptr_release(this);
                }

                shutdown_helper(): last_worker(nullptr) {}

                std::mutex mtx;
                std::condition_variable cv;
                scheduler_abstract_t* last_worker;
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
                { // lifetime scope of guard
                    std::unique_lock<std::mutex> guard(sh.mtx);
                    sh.cv.wait(guard, [&] { return sh.last_worker != nullptr; });
                }
                alive_workers.erase(it);
                sh.last_worker = nullptr;
            }

            for (auto& w : workers_) {
                w->get_thread().join(); /// wait until all workers finish working
            }

            /// run cleanup code for each resumable
            auto f = &cleanup_and_release;
            for (auto& w : workers_) {
                policy_.foreach_resumable(w.get(), f);
            }
            policy_.foreach_central_resumable(this, f);
        }

        void enqueue(resumable* ptr) override {
            intrusive_ptr_add_ref(ptr);  // Take ownership before scheduling
            policy_.central_enqueue(this, ptr);
        }

    private:
        std::vector<std::unique_ptr<worker_type>> workers_;
        policy_data data_;
        Policy policy_;
    };

}} // namespace actor_zeta::scheduler