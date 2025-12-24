#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <actor-zeta.hpp>

class worker_actor final : public actor_zeta::basic_actor<worker_actor> {
public:
    actor_zeta::unique_future<void> process_task(std::string task);
    actor_zeta::unique_future<void> get_status();

    using dispatch_traits = actor_zeta::dispatch_traits<
        &worker_actor::process_task,
        &worker_actor::get_status
    >;

    explicit worker_actor(std::pmr::memory_resource* ptr, std::string name)
        : actor_zeta::basic_actor<worker_actor>(ptr)
        , name_(std::move(name)) {
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<worker_actor, &worker_actor::process_task>:
                actor_zeta::dispatch(this, &worker_actor::process_task, msg);
                break;
            case actor_zeta::msg_id<worker_actor, &worker_actor::get_status>:
                actor_zeta::dispatch(this, &worker_actor::get_status, msg);
                break;
        }
    }

    std::string name() const { return name_; }
    size_t tasks_processed() const { return tasks_processed_; }

private:
    std::string name_;
    size_t tasks_processed_ = 0;
};

actor_zeta::unique_future<void> worker_actor::process_task(std::string task) {
    std::cerr << "[" << name_ << "] Processing task: " << task << std::endl;
    ++tasks_processed_;
    co_return;
}

actor_zeta::unique_future<void> worker_actor::get_status() {
    std::cerr << "[" << name_ << "] Processed " << tasks_processed_ << " tasks" << std::endl;
    co_return;
}

class supervisor_actor final : public actor_zeta::actor::actor_mixin<supervisor_actor> {
public:
    template<typename T> using unique_future = actor_zeta::unique_future<T>;

    supervisor_actor(std::pmr::memory_resource* ptr, actor_zeta::scheduler::sharing_scheduler* scheduler)
        : actor_zeta::actor::actor_mixin<supervisor_actor>()
        , resource_(ptr)
        , scheduler_(scheduler) {
    }

    std::pmr::memory_resource* resource() const noexcept { return resource_; }

    actor_zeta::unique_future<void> create_worker(std::string name) {
        auto worker = actor_zeta::spawn<worker_actor>(resource_, name);
        std::cerr << "[Supervisor] Created worker: " << name << std::endl;
        workers_.emplace_back(std::move(worker));
        co_return;
    }

    actor_zeta::unique_future<void> assign_task(std::string task) {
        if (workers_.empty()) {
            std::cerr << "[Supervisor] No workers available!" << std::endl;
            co_return;
        }

        auto& worker = workers_[next_worker_];
        next_worker_ = (next_worker_ + 1) % workers_.size();

        std::cerr << "[Supervisor] Assigning task '" << task << "' to " << worker->name() << std::endl;

        auto future = actor_zeta::send(worker.get(), address(), &worker_actor::process_task, task);
        if (future.needs_scheduling()) {
            scheduler_->enqueue(worker.get());
        }
        co_return;
    }

    actor_zeta::unique_future<void> stop_workers() {
        std::cerr << "[Supervisor] Stopping all workers..." << std::endl;
        co_return;
    }

    actor_zeta::unique_future<void> check_status() {
        std::cerr << "[Supervisor] Status check - " << workers_.size() << " workers:" << std::endl;
        for (auto& worker : workers_) {
            auto future = actor_zeta::send(worker.get(), address(), &worker_actor::get_status);
            if (future.needs_scheduling()) {
                scheduler_->enqueue(worker.get());
            }
        }
        co_return;
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<supervisor_actor, &supervisor_actor::create_worker>) {
            actor_zeta::dispatch(this, &supervisor_actor::create_worker, msg);
        } else if (cmd == actor_zeta::msg_id<supervisor_actor, &supervisor_actor::assign_task>) {
            actor_zeta::dispatch(this, &supervisor_actor::assign_task, msg);
        } else if (cmd == actor_zeta::msg_id<supervisor_actor, &supervisor_actor::stop_workers>) {
            actor_zeta::dispatch(this, &supervisor_actor::stop_workers, msg);
        } else if (cmd == actor_zeta::msg_id<supervisor_actor, &supervisor_actor::check_status>) {
            actor_zeta::dispatch(this, &supervisor_actor::check_status, msg);
        }
    }

    size_t worker_count() const { return workers_.size(); }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &supervisor_actor::create_worker,
        &supervisor_actor::assign_task,
        &supervisor_actor::stop_workers,
        &supervisor_actor::check_status
    >;

    template<typename ReturnType, typename... Args>
    ReturnType enqueue_impl(
        actor_zeta::actor::address_t sender,
        actor_zeta::mailbox::message_id cmd,
        Args&&... args
    ) {
        using R = typename actor_zeta::type_traits::is_unique_future<ReturnType>::value_type;
        return enqueue_sync_impl<R>(
            sender,
            cmd,
            [this](auto* msg) { behavior(msg); },
            std::forward<Args>(args)...
        );
    }

protected:

private:
    std::pmr::memory_resource* resource_;
    actor_zeta::scheduler::sharing_scheduler* scheduler_;
    std::vector<std::unique_ptr<worker_actor, actor_zeta::pmr::deleter_t>> workers_;
    size_t next_worker_ = 0;
    std::mutex mutex_;
};

int main() {
    auto* resource =std::pmr::get_default_resource();

    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(2, 1000));
    scheduler->start();

    auto supervisor = actor_zeta::spawn<supervisor_actor>(resource, scheduler.get());

    std::cerr << "=== Supervisor Example: Manual Scheduling ===" << std::endl;
    std::cerr << std::endl;

    std::cerr << "--- Creating Workers ---" << std::endl;
    for (int i = 1; i <= 3; ++i) {
        actor_zeta::send(
            supervisor.get(),
            actor_zeta::address_t::empty_address(),
            &supervisor_actor::create_worker,
            std::string("Worker-") + std::to_string(i));
    }
    std::cerr << std::endl;

    std::cerr << "--- Assigning Tasks ---" << std::endl;
    std::vector<std::string> tasks = {
        "Parse config file",
        "Process HTTP request",
        "Update database",
        "Send notification",
        "Generate report",
        "Clean temp files"
    };

    for (const auto& task : tasks) {
        actor_zeta::send(
            supervisor.get(),
            actor_zeta::address_t::empty_address(),
            &supervisor_actor::assign_task,
            task);
    }
    std::cerr << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cerr << "--- Checking Status ---" << std::endl;
    {
        actor_zeta::send(
            supervisor.get(),
            actor_zeta::address_t::empty_address(),
            &supervisor_actor::check_status);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::cerr << std::endl;
    std::cerr << "--- Stopping ---" << std::endl;
    {
        actor_zeta::send(
            supervisor.get(),
            actor_zeta::address_t::empty_address(),
            &supervisor_actor::stop_workers);
    }

    scheduler->stop();

    std::cerr << std::endl;
    std::cerr << "=== Example Complete ===" << std::endl;

    return 0;
}