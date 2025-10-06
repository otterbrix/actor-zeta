#pragma once

#include <cassert>

#include <list>
#include <set>

#include <actor-zeta/detail/launch_thread.hpp>
#include <actor-zeta/detail/memory.hpp>
#include <actor-zeta/scheduler/scheduler.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>

namespace {

    template<class Parent>
    class worker final : public actor_zeta::scheduler::scheduler_t {
    public:
        using job_ptr = actor_zeta::scheduler::resumable_t*;

        worker(size_t worker_id, Parent* parent, size_t throughput)
            : max_throughput_(throughput)
            , parent_{parent}
            , id_(worker_id) {
        }

        worker(const worker&) = delete;
        worker& operator=(const worker&) = delete;

        void start() override {
            assert(this_thread_.get_id() == std::thread::id{}); /// TODO: see implement asio
            this_thread_ = actor_zeta::detail::launch_thread("worker", [this] { run(); });
        }

        void stop() override {}

        void schedule(job_ptr job) override {
            assert(job != nullptr);
            parent_->schedule(job);
        }

        size_t id() const noexcept {
            return id_;
        }

        std::thread& get_thread() noexcept {
            return this_thread_;
        }

        size_t max_throughput() noexcept {
            return max_throughput_;
        }

    private:
        void run() {
            for (;;) {
                auto job = parent_->dequeue();
                assert(job != nullptr);
                auto res = job->resume(this, max_throughput_);
                switch (res) {
                    case actor_zeta::scheduler::resume_result::resume:
                        parent_->schedule(job);
                        break;
                    case actor_zeta::scheduler::resume_result::awaiting:
                    case actor_zeta::scheduler::resume_result::done:
                        intrusive_ptr_release(job);
                        break;
                    case actor_zeta::scheduler::resume_result::shutdown: {
                        return;
                    }
                }
            }
        }

        size_t max_throughput_;
        std::thread this_thread_;
        Parent* parent_;
        size_t id_;
    };

    void cleanup_and_release(actor_zeta::scheduler::resumable_t* ptr) {
        class dummy_scheduler final : public actor_zeta::scheduler_t {
        public:
            void schedule(actor_zeta::scheduler::resumable_t* job) override {
                resumables.push_back(job);
            }

            void start() override {}
            void stop() override {}

            std::vector<actor_zeta::scheduler::resumable_t*> resumables;
        };

        {
            dummy_scheduler dummy;
            while (!dummy.resumables.empty()) {
                /* ??? auto sub =  ??? */ dummy.resumables.back();
                dummy.resumables.pop_back();
            }
        }
        intrusive_ptr_release(ptr);
    }

    class sharing_scheduler_impl final : public actor_zeta::scheduler::scheduler_t {
    public:
        using worker_type = worker<sharing_scheduler_impl>;
        using queue_type = std::list<actor_zeta::scheduler::resumable_t*>;

        sharing_scheduler_impl(std::size_t num_worker_threads, std::size_t max_throughput)
            : max_throughput_(max_throughput)
            , num_workers_(num_worker_threads) {
        }

        size_t num_workers() const noexcept {
            return num_workers_;
        }

        void schedule(actor_zeta::scheduler::resumable_t* ptr) override {
            queue_type l;
            l.push_back(ptr);
            std::unique_lock<std::mutex> guard(lock);
            queue.splice(queue.end(), l);
            cv.notify_one();
        }

        void start() override {
            auto num = num_workers_;
            workers_.reserve(num);
            for (size_t i = 0; i < num; ++i) {
                workers_.emplace_back(std::make_unique<worker_type>(i, this, max_throughput_));
            }
            for (auto& w : workers_) {
                w->start();
            }
        }

        void stop() override {
            // Shutdown workers.
            class shutdown_helper final
                : public actor_zeta::scheduler::resumable_t
                , public actor_zeta::ref_counted {
            public:
                actor_zeta::scheduler::resume_result resume(scheduler_t* ptr, size_t) override {
                    assert(ptr != nullptr);
                    std::unique_lock<std::mutex> guard(mtx);
                    last_worker = ptr;
                    cv.notify_all();
                    return actor_zeta::scheduler::resume_result::shutdown;
                }

                void intrusive_ptr_add_ref_impl() override final {
                    intrusive_ptr_add_ref(this);
                }

                void intrusive_ptr_release_impl() override final {
                    intrusive_ptr_release(this);
                }
                shutdown_helper()
                    : last_worker(nullptr) {
                    // nop
                }
                std::mutex mtx;
                std::condition_variable cv;
                scheduler_t* last_worker;
            };
            // Use a set to keep track of remaining workers.
            shutdown_helper sh;
            std::set<worker_type*> alive_workers;
            for (size_t i = 0; i < num_workers_; ++i) {
                alive_workers.insert(worker_by_id(i));
                sh.ref(); // Make sure reference count is high enough.
            }
            while (!alive_workers.empty()) {
                (*alive_workers.begin())->schedule(&sh);
                {
                    std::unique_lock<std::mutex> guard(sh.mtx);
                    sh.cv.wait(guard, [&] { return sh.last_worker != nullptr; });
                }
                alive_workers.erase(static_cast<worker_type*>(sh.last_worker));
                sh.last_worker = nullptr;
            }

            for (auto& w : workers_) {
                w->get_thread().join();
            }

            foreach_central_resumable(cleanup_and_release);
        }

        actor_zeta::scheduler::resumable_t* dequeue() {
            std::unique_lock<std::mutex> guard(lock);
            cv.wait(guard, [&] { return !queue.empty(); });
            actor_zeta::scheduler::resumable_t* job = queue.front();
            queue.pop_front();
            return job;
        }

    private:
        worker_type* worker_by_id(size_t x) {
            return workers_[x].get();
        }

        template<class UnaryFunction>
        void foreach_central_resumable(UnaryFunction f) {
            auto next = [&]() -> actor_zeta::scheduler::resumable_t* {
                if (queue.empty()) {
                    return nullptr;
                }
                auto front = queue.front();
                queue.pop_front();
                return front;
            };
            std::unique_lock<std::mutex> guard(lock);
            for (auto job = next(); job != nullptr; job = next()) {
                f(job);
            }
        }

        std::vector<std::unique_ptr<worker_type>> workers_;
        queue_type queue;
        std::mutex lock;
        std::condition_variable cv;
        size_t max_throughput_ = 0;
        size_t num_workers_ = 0;
    };

} // namespace

namespace actor_zeta { namespace scheduler {

    std::unique_ptr<scheduler_t, pmr::deleter_t> make_sharing_scheduler(
        actor_zeta::pmr::memory_resource* resource,
        std::size_t num_worker_threads,
        std::size_t max_throughput) {
        return {pmr::allocate_ptr<sharing_scheduler_impl>(resource, num_worker_threads, max_throughput), actor_zeta::pmr::deleter_t(resource)};
    }

}} // namespace actor_zeta::scheduler