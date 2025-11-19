#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/scheduler.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>

// Simple worker actor that processes tasks
class worker_actor final : public actor_zeta::basic_actor<worker_actor> {
public:
    actor_zeta::unique_future<void> process_task(const std::string& task);
    actor_zeta::unique_future<void> get_status();

    using dispatch_traits = actor_zeta::dispatch_traits<
        &worker_actor::process_task,
        &worker_actor::get_status
    >;

    explicit worker_actor(actor_zeta::pmr::memory_resource* ptr, std::string name)
        : actor_zeta::basic_actor<worker_actor>(ptr)
        , name_(std::move(name))
        , process_task_(actor_zeta::make_behavior(resource(), this, &worker_actor::process_task))
        , get_status_(actor_zeta::make_behavior(resource(), this, &worker_actor::get_status)) {
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<worker_actor, &worker_actor::process_task>:
                process_task_(msg);
                break;
            case actor_zeta::msg_id<worker_actor, &worker_actor::get_status>:
                get_status_(msg);
                break;
        }
    }

    std::string name() const { return name_; }
    size_t tasks_processed() const { return tasks_processed_; }

private:
    std::string name_;
    size_t tasks_processed_ = 0;
    actor_zeta::behavior_t process_task_;
    actor_zeta::behavior_t get_status_;
};

actor_zeta::unique_future<void> worker_actor::process_task(const std::string& task) {
    std::cerr << "[" << name_ << "] Processing task: " << task << std::endl;
    ++tasks_processed_;
    return actor_zeta::make_ready_future_void(resource());
}

actor_zeta::unique_future<void> worker_actor::get_status() {
    std::cerr << "[" << name_ << "] Processed " << tasks_processed_ << " tasks" << std::endl;
    return actor_zeta::make_ready_future_void(resource());
}

// Supervisor that manages worker actors with manual scheduling
class supervisor_actor final : public actor_zeta::base::actor_mixin<supervisor_actor> {
public:
    template<typename T> using unique_future = actor_zeta::unique_future<T>;

    supervisor_actor(actor_zeta::pmr::memory_resource* ptr, actor_zeta::scheduler::sharing_scheduler* scheduler)
        : actor_zeta::base::actor_mixin<supervisor_actor>()
        , resource_(ptr)
        , scheduler_(scheduler)
        , create_worker_(actor_zeta::make_behavior(resource_, this, &supervisor_actor::create_worker))
        , assign_task_(actor_zeta::make_behavior(resource_, this, &supervisor_actor::assign_task))
        , stop_workers_(actor_zeta::make_behavior(resource_, this, &supervisor_actor::stop_workers))
        , check_status_(actor_zeta::make_behavior(resource_, this, &supervisor_actor::check_status)) {
    }

    actor_zeta::pmr::memory_resource* resource() const noexcept { return resource_; }

    actor_zeta::unique_future<void> create_worker(const std::string& name) {
        auto worker = actor_zeta::spawn<worker_actor>(resource_, name);
        std::cerr << "[Supervisor] Created worker: " << name << std::endl;
        workers_.emplace_back(std::move(worker));
        return actor_zeta::make_ready_future_void(resource_);
    }

    actor_zeta::unique_future<void> assign_task(const std::string& task) {
        if (workers_.empty()) {
            std::cerr << "[Supervisor] No workers available!" << std::endl;
            return actor_zeta::make_ready_future_void(resource_);
        }

        // Round-robin task distribution
        auto& worker = workers_[next_worker_];
        next_worker_ = (next_worker_ + 1) % workers_.size();

        std::cerr << "[Supervisor] Assigning task '" << task << "' to " << worker->name() << std::endl;

        // Send task to worker (returns future with needs_scheduling flag)
        auto future = actor_zeta::send(worker.get(), address(), &worker_actor::process_task, task);

        // Manual scheduling - only if actor was unblocked (check needs_scheduling)
        if (future.needs_scheduling()) {
            scheduler_->enqueue(worker.get());
        }
        return actor_zeta::make_ready_future_void(resource_);
    }

    actor_zeta::unique_future<void> stop_workers() {
        std::cerr << "[Supervisor] Stopping all workers..." << std::endl;
        // Workers will be automatically destroyed when supervisor is destroyed
        // In real system, you'd send shutdown messages and wait
        return actor_zeta::make_ready_future_void(resource_);
    }

    actor_zeta::unique_future<void> check_status() {
        std::cerr << "[Supervisor] Status check - " << workers_.size() << " workers:" << std::endl;
        for (auto& worker : workers_) {
            // Send status request to worker (returns future with needs_scheduling flag)
            auto future = actor_zeta::send(worker.get(), address(), &worker_actor::get_status);

            // Manual scheduling - only if actor was unblocked (check needs_scheduling)
            if (future.needs_scheduling()) {
                scheduler_->enqueue(worker.get());
            }
        }
        return actor_zeta::make_ready_future_void(resource_);
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<supervisor_actor, &supervisor_actor::create_worker>) {
            create_worker_(msg);
        } else if (cmd == actor_zeta::msg_id<supervisor_actor, &supervisor_actor::assign_task>) {
            assign_task_(msg);
        } else if (cmd == actor_zeta::msg_id<supervisor_actor, &supervisor_actor::stop_workers>) {
            stop_workers_(msg);
        } else if (cmd == actor_zeta::msg_id<supervisor_actor, &supervisor_actor::check_status>) {
            check_status_(msg);
        }
    }

    size_t worker_count() const { return workers_.size(); }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &supervisor_actor::create_worker,
        &supervisor_actor::assign_task,
        &supervisor_actor::stop_workers,
        &supervisor_actor::check_status
    >;

    // NEW API: Forward arguments to enqueue_sync_impl (message created in receiver's resource)
    template<typename R, typename... Args>
    unique_future<R> enqueue_impl(
        actor_zeta::base::address_t sender,
        actor_zeta::mailbox::message_id cmd,
        Args&&... args
    ) {
        return enqueue_sync_impl<R>(
            sender,
            cmd,
            [this](auto* msg) { behavior(msg); },
            std::forward<Args>(args)...
        );
    }

protected:

private:
    actor_zeta::pmr::memory_resource* resource_;
    actor_zeta::scheduler::sharing_scheduler* scheduler_;
    std::vector<std::unique_ptr<worker_actor, actor_zeta::pmr::deleter_t>> workers_;
    size_t next_worker_ = 0;
    std::mutex mutex_;

    actor_zeta::behavior_t create_worker_;
    actor_zeta::behavior_t assign_task_;
    actor_zeta::behavior_t stop_workers_;
    actor_zeta::behavior_t check_status_;
};

int main() {
    auto* resource = actor_zeta::pmr::get_default_resource();

    // Create scheduler with 2 threads and throughput of 1000 messages per resume
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(2, 1000));
    scheduler->start();

    // Create supervisor
    auto supervisor = actor_zeta::spawn<supervisor_actor>(resource, scheduler.get());

    std::cerr << "=== Supervisor Example: Manual Scheduling ===" << std::endl;
    std::cerr << std::endl;

    // Create workers
    std::cerr << "--- Creating Workers ---" << std::endl;
    for (int i = 1; i <= 3; ++i) {
        actor_zeta::send(
            supervisor.get(),
            actor_zeta::address_t::empty_address(),
            &supervisor_actor::create_worker,
            std::string("Worker-") + std::to_string(i));
    }
    std::cerr << std::endl;

    // Assign tasks
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

    // Give workers time to process
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Check status
    std::cerr << "--- Checking Status ---" << std::endl;
    {
        actor_zeta::send(
            supervisor.get(),
            actor_zeta::address_t::empty_address(),
            &supervisor_actor::check_status);
    }

    // Give time for status check
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Stop
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