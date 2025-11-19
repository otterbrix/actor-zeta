#include <cassert>
#include <iostream>
#include <memory>
#include <vector>

#include <actor-zeta.hpp>
#include <actor-zeta/detail/memory.hpp>
#include <actor-zeta/scheduler/scheduler.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>

using actor_zeta::pmr::memory_resource;

class worker_t final : public actor_zeta::basic_actor<worker_t> {
public:
    // Request-response methods (return results automatically via promise)
    actor_zeta::unique_future<std::size_t> download_with_result(const std::string& url, const std::string& /*user*/, const std::string& /*password*/);
    actor_zeta::unique_future<std::size_t> work_data_with_result(const std::string& data, const std::string& /*operatorName*/);

    using dispatch_traits = actor_zeta::dispatch_traits<
        &worker_t::download_with_result,
        &worker_t::work_data_with_result
    >;

    worker_t(actor_zeta::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<worker_t>(ptr)
        , download_with_result_(actor_zeta::make_behavior(resource(), this, &worker_t::download_with_result))
        , work_data_with_result_(actor_zeta::make_behavior(resource(), this, &worker_t::work_data_with_result)) {
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        std::cerr << "[Worker " << id() << "] behavior() called, cmd=" << cmd
                  << ", error_before=" << static_cast<int>(msg->error()) << std::endl;

        switch (cmd) {
            case actor_zeta::msg_id<worker_t, &worker_t::download_with_result>: {
                download_with_result_(msg);
                std::cerr << "[Worker " << id() << "] After handler, error=" << static_cast<int>(msg->error()) << std::endl;
                break;
            }
            case actor_zeta::msg_id<worker_t, &worker_t::work_data_with_result>: {
                work_data_with_result_(msg);
                break;
            }
        }
    }

private:
    actor_zeta::behavior_t download_with_result_;
    actor_zeta::behavior_t work_data_with_result_;
    std::string tmp_;
};

// Request-response implementations - automatically return results via promise
inline actor_zeta::unique_future<std::size_t> worker_t::download_with_result(const std::string& url, const std::string& /*user*/, const std::string& /*password*/) {
    std::cerr << "[Worker " << id() << "] Processing download_with_result: " << url << std::endl;
    tmp_ = url;
    std::cerr << "[Worker " << id() << "] Returning size: " << tmp_.size() << std::endl;
    std::size_t result = tmp_.size(); // Return downloaded size
    return actor_zeta::make_ready_future<std::size_t>(resource(), result);
}

inline actor_zeta::unique_future<std::size_t> worker_t::work_data_with_result(const std::string& data, const std::string& /*operatorName*/) {
    tmp_ = data;
    std::size_t result = tmp_.size(); // Return processed size
    return actor_zeta::make_ready_future<std::size_t>(resource(), result);
}

/// non thread safe
class supervisor_lite final : public actor_zeta::base::actor_mixin<supervisor_lite> {
public:
    template<typename T> using unique_future = actor_zeta::unique_future<T>;

    supervisor_lite(memory_resource* ptr)
        : actor_zeta::base::actor_mixin<supervisor_lite>()
        , resource_(ptr)
        , create_(actor_zeta::make_behavior(resource_,  this, &supervisor_lite::create))
        , e_(new actor_zeta::scheduler::sharing_scheduler(2, 1000)) {
        e_->start();
    }

    actor_zeta::pmr::memory_resource* resource() const noexcept { return resource_; }

    ~supervisor_lite() {
        e_->stop();
        delete e_;
    }

    actor_zeta::unique_future<void> create() {
        auto ptr = actor_zeta::spawn<worker_t>(resource_);
        actors_.emplace_back(std::move(ptr));
        ++size_actors_;
        return actor_zeta::make_ready_future_void(resource_);
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<supervisor_lite, &supervisor_lite::create>) {
            create_(msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &supervisor_lite::create
    >;

    // Public access to workers for direct send() with futures
    worker_t* get_worker(std::size_t index) {
        return index < actors_.size() ? actors_[index].get() : nullptr;
    }

    std::size_t worker_count() const noexcept {
        return size_actors_.load();
    }

    // Schedule worker for execution
    void schedule_worker(std::size_t index) {
        if (index < actors_.size()) {
            e_->enqueue(actors_[index].get());
        }
    }

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
    std::size_t size_actor() noexcept {
        return size_actors_.load();
    }

    actor_zeta::pmr::memory_resource* resource_;
    actor_zeta::behavior_t create_;
    actor_zeta::scheduler::sharing_scheduler* e_;
    std::vector<std::unique_ptr<worker_t, actor_zeta::pmr::deleter_t>> actors_;
    std::atomic<int64_t> size_actors_{0};
    std::mutex mutex_;
};

int main() {
    auto* mr_ptr = actor_zeta::pmr::get_default_resource();
    auto supervisor = actor_zeta::spawn<supervisor_lite>(mr_ptr);

    int const actors = 5;

    // Create actors using new send() API (supervisor processes messages synchronously)
    // Collect futures from create() calls and wait for completion
    std::vector<supervisor_lite::unique_future<void>> create_futures;
    create_futures.reserve(actors);
    for (auto i = actors; i > 0; --i) {
        create_futures.push_back(actor_zeta::send(supervisor.get(), actor_zeta::address_t::empty_address(), &supervisor_lite::create));
    }

    // Wait for all actors to be created
    for (auto& future : create_futures) {
        std::move(future).get();
    }

    std::cerr << "=== Created " << actors << " worker actors ===" << std::endl;

    // Demonstrate request-response pattern with futures
    std::cerr << "\n=== REQUEST-RESPONSE PATTERN WITH FUTURES ===" << std::endl;
    std::cerr << "Collecting futures from multiple workers..." << std::endl;

    std::vector<worker_t::unique_future<std::size_t>> futures;
    futures.reserve(supervisor->worker_count()); // CRITICAL: Reserve to avoid reallocation!
    for (std::size_t i = 0; i < supervisor->worker_count(); ++i) {
        auto* worker = supervisor->get_worker(i);
        if (worker) {
            auto future = actor_zeta::send(
                worker,
                actor_zeta::address_t::empty_address(),
                &worker_t::download_with_result,
                std::string("test_data_" + std::to_string(i)),
                std::string("user"),
                std::string("pass")
            );

            if (future.needs_scheduling()) {
                supervisor->schedule_worker(i);
            }
            futures.push_back(std::move(future));
        }
    }

    std::cerr << "  Sent " << futures.size() << " requests, calling get() on each..." << std::endl;

    // Collect results using blocking get()
    std::size_t total_size = 0;
    for (std::size_t i = 0; i < futures.size(); ++i) {
        std::size_t result = std::move(futures[i]).get();
        total_size += result;
        std::cerr << "  Worker " << i << " returned: " << result << std::endl;
    }

    std::cerr << "  Total size from all workers: " << total_size << std::endl;

    std::cerr << "\n=== Request-response test: PASSED ✓ ===" << std::endl;

    return 0;
}
