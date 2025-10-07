#include <cassert>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include <actor-zeta.hpp>
#include <actor-zeta/detail/memory.hpp>
#include <actor-zeta/scheduler/scheduler.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>

auto thread_pool_deleter = [](actor_zeta::scheduler_t* ptr) {
    ptr->stop();
    delete ptr;
};

static std::atomic<uint64_t> counter_download_data{0};
static std::atomic<uint64_t> counter_work_data{0};

using actor_zeta::pmr::memory_resource;

class worker_t final : public actor_zeta::basic_actor<worker_t> {
public:
    enum class command_t : uint64_t {
        download = 0x00,
        work_data
    };

    worker_t(actor_zeta::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<worker_t>(ptr)
        , download_(actor_zeta::make_behavior(resource(), this, &worker_t::download))
        , work_data_(actor_zeta::make_behavior(resource(),  this, &worker_t::work_data)) {
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::make_message_id(command_t::download): {
                download_(msg);
                break;
            }
            case actor_zeta::make_message_id(command_t::work_data): {
                work_data_(msg);
                break;
            }
        }
    }


    void download(const std::string& url, const std::string& /*user*/, const std::string& /*password*/) {
        tmp_ = url;
        counter_download_data++;
    }

    void work_data(const std::string& data, const std::string& /*operatorName*/) {
        tmp_ = data;
        counter_work_data++;
    }

private:
    actor_zeta::behavior_t download_;
    actor_zeta::behavior_t work_data_;
    std::string tmp_;
};

/// non thread safe
class supervisor_lite final : public actor_zeta::actor_abstract_t {
public:
    enum class system_command : std::uint64_t {
        broadcast = 0x00,
        create
    };

    supervisor_lite(memory_resource* ptr)
        : actor_zeta::actor_abstract_t(ptr)
        , create_(actor_zeta::make_behavior(resource(),  this, &supervisor_lite::create))
        , broadcast_(actor_zeta::make_behavior(resource(), this, &supervisor_lite::broadcast_impl))
        , e_(actor_zeta::scheduler::make_sharing_scheduler(ptr, 2, 1000)) {
        e_->start();
    }

    ~supervisor_lite() {
        e_->stop();
    }

    void create() {
        auto ptr = actor_zeta::spawn<worker_t>(resource());
        actors_.emplace_back(std::move(ptr));
        ++size_actors_;
    }

    void  behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::make_message_id(system_command::create): {
                create_(msg);
                break;
            }
            case actor_zeta::make_message_id(system_command::broadcast): {
                broadcast_(msg);
                break;
            }
        }
    }

    template<class Id = uint64_t, class... Args>
    void broadcast_on_worker(Id id, Args... args) {
        auto end = size_actor();

        if (end == 0) {
            return;
        }

        // Direct broadcast - first enqueue to all actors
        for (std::size_t i = 0; i < end; ++i) {
            auto msg = actor_zeta::make_message(
                resource(),
                address(),
                id,
                args...);
            actors_[i]->enqueue(std::move(msg));
        }

        // Then schedule all actors once
        for (std::size_t i = 0; i < end; ++i) {
            e_->schedule(actors_[i].get());
        }
    }

protected:
    bool enqueue_impl(actor_zeta::message_ptr msg) override {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tmp = std::move(msg);
            behavior(tmp.get());
        }
        return true;
    }

private:
    std::size_t size_actor() noexcept {
        return size_actors_.load();
    }

    void broadcast_impl(std::vector<actor_zeta::message_ptr> msg) {
        auto msgs = std::move(msg);
        auto end = size_actor();

        if (end == 0) {
            std::cerr << "Warning: no actors to broadcast to!" << std::endl;
            return;
        }

        // Send all messages first
        for (std::size_t i = 0; i != end; ++i) {
            actors_[i]->enqueue(std::move(msgs[i]));
        }
        // Then schedule all actors once
        for (std::size_t i = 0; i != end; ++i) {
            e_->schedule(actors_[i].get());
        }
    }

    actor_zeta::behavior_t create_;
    actor_zeta::behavior_t broadcast_;
    std::unique_ptr<actor_zeta::scheduler_t, actor_zeta::pmr::deleter_t> e_;
    std::vector<std::unique_ptr<worker_t, actor_zeta::pmr::deleter_t>> actors_;
    std::atomic<int64_t> size_actors_{0};
    std::mutex mutex_;
};

int main() {
    auto* mr_ptr = actor_zeta::pmr::get_default_resource();
    auto supervisor = actor_zeta::spawn<supervisor_lite>(mr_ptr);

    int const actors = 5;

    // Create actors directly (supervisor processes messages synchronously)
    for (auto i = actors; i > 0; --i) {
        auto msg = actor_zeta::make_message(
            mr_ptr,
            actor_zeta::address_t::empty_address(),
            supervisor_lite::system_command::create);
        supervisor->enqueue(std::move(msg));
    }

    std::cerr << "=== Created " << actors << " worker actors ===" << std::endl;

    // Broadcast download task - one message to all actors
    std::cerr << "=== Broadcasting download task to all " << actors << " actors ===" << std::endl;
    supervisor->broadcast_on_worker(worker_t::command_t::download, std::string("url"), std::string("user"), std::string("pass"));

    // Broadcast work_data task - one message to all actors
    std::cerr << "=== Broadcasting work_data task to all " << actors << " actors ===" << std::endl;
    supervisor->broadcast_on_worker(worker_t::command_t::work_data, std::string("data"), std::string("operator"));

    std::cerr << "=== Waiting for processing ===" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cerr << "\n=== Results ===" << std::endl;
    std::cerr << "Download messages processed: " << counter_download_data.load() << std::endl;
    std::cerr << "Work_data messages processed: " << counter_work_data.load() << std::endl;

    auto expected = actors; // One broadcast per type, all actors should process
    std::cerr << "Expected per counter: " << expected << std::endl;

    // Success if all broadcasts reached all actors
    bool passed = counter_download_data.load() == expected && counter_work_data.load() == expected;
    std::cerr << "\nTest result: " << (passed ? "PASSED ✓" : "FAILED ✗") << std::endl;

    return passed ? 0 : 1;
}
