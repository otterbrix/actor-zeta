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
    void broadcast_on_worker(Id id, Args&&... args) {
        auto size = size_actor();
        std::vector<actor_zeta::message_ptr> tmp;
        tmp.reserve(size);
        for (std::size_t i = 0, e = size; i != e; ++i) {
            auto ptr = actor_zeta::make_message(
                resource(),
                address(),
                id,
                std::forward<const Args&>(args)...);
            tmp.push_back(std::move(ptr));
        }

        auto msg = actor_zeta::make_message(
            resource(),
            address(),
            system_command::broadcast,
            std::move(tmp));
        enqueue(std::move(msg));
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
        for (std::size_t i = 0, end = size_actor(); i != end; ++i) {
            actor_zeta::send(actors_[i].get(), std::move(msgs[i]));
        }
        for (std::size_t i = 0, end = size_actor(); i != end; ++i) {
            actor_zeta::enqueue(e_.get(),(actors_[i].get()));
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

    int const actors = 10;

    for (auto i = actors - 1; i > 0; --i) {
        actor_zeta::send(supervisor.get(), actor_zeta::address_t::empty_address(), supervisor_lite::system_command::create);
    }

    int const task = 10000;

    for (auto i = task - 1; i > 0; --i) {
        supervisor->broadcast_on_worker(worker_t::command_t::download, std::string("fb"), std::string("jack"), std::string("1"));
    }

    for (auto i = task - 1; i > 0; --i) {
        supervisor->broadcast_on_worker(worker_t::command_t::work_data, std::string("fb"), std::string("jack"));
    }

    std::this_thread::sleep_for(std::chrono::seconds(180));

    std::cerr << " Finish " << std::endl;
    std::cerr << "counter_download_data :" << counter_download_data.load() << std::endl;
    std::cerr << "counter_work_data :" << counter_work_data.load() << std::endl;
    assert(counter_download_data.load() == 9999);
    assert(counter_work_data.load() == 9999);
    return 0;
}
