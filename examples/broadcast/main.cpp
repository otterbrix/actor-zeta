#include <cassert>
#include <iostream>
#include <memory>
#include <vector>

#include <actor-zeta.hpp>

using std::pmr::memory_resource;

class worker_t final : public actor_zeta::basic_actor<worker_t> {
public:
    actor_zeta::unique_future<std::size_t> download_with_result(std::string url, std::string /*user*/, std::string /*password*/);
    actor_zeta::unique_future<std::size_t> work_data_with_result(std::string data, std::string /*operatorName*/);

    using dispatch_traits = actor_zeta::dispatch_traits<
        &worker_t::download_with_result,
        &worker_t::work_data_with_result
    >;

    worker_t(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<worker_t>(ptr) {
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        switch (cmd) {
            case actor_zeta::msg_id<worker_t, &worker_t::download_with_result>:
                actor_zeta::dispatch(this, &worker_t::download_with_result, msg);
                break;
            case actor_zeta::msg_id<worker_t, &worker_t::work_data_with_result>:
                actor_zeta::dispatch(this, &worker_t::work_data_with_result, msg);
                break;
        }
    }

private:
    std::string tmp_;
};

inline actor_zeta::unique_future<std::size_t> worker_t::download_with_result(std::string url, std::string /*user*/, std::string /*password*/) {
    tmp_ = std::move(url);
    co_return tmp_.size();
}

inline actor_zeta::unique_future<std::size_t> worker_t::work_data_with_result(std::string data, std::string /*operatorName*/) {
    tmp_ = std::move(data);
    co_return tmp_.size();
}

class supervisor_lite final : public actor_zeta::actor::actor_mixin<supervisor_lite> {
public:
    template<typename T> using unique_future = actor_zeta::unique_future<T>;

    supervisor_lite(memory_resource* ptr)
        : actor_zeta::actor::actor_mixin<supervisor_lite>()
        , resource_(ptr)
        , e_(new actor_zeta::sharing_scheduler(2, 1000)) {
        e_->start();
    }

    std::pmr::memory_resource* resource() const noexcept { return resource_; }

    ~supervisor_lite() {
        e_->stop();
        delete e_;
    }

    actor_zeta::unique_future<void> create() {
        auto ptr = actor_zeta::spawn<worker_t>(resource_);
        actors_.emplace_back(std::move(ptr));
        ++size_actors_;
        co_return;
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<supervisor_lite, &supervisor_lite::create>) {
            actor_zeta::dispatch(this, &supervisor_lite::create, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &supervisor_lite::create
    >;

    worker_t* get_worker(std::size_t index) {
        return index < actors_.size() ? actors_[index].get() : nullptr;
    }

    std::size_t worker_count() const noexcept {
        return size_actors_.load();
    }

    void schedule_worker(std::size_t index) {
        if (index < actors_.size()) {
            e_->enqueue(actors_[index].get());
        }
    }

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
    std::size_t size_actor() noexcept {
        return size_actors_.load();
    }

    std::pmr::memory_resource* resource_;
    actor_zeta::sharing_scheduler* e_;
    std::vector<std::unique_ptr<worker_t, actor_zeta::pmr::deleter_t>> actors_;
    std::atomic<int64_t> size_actors_{0};
    std::mutex mutex_;
};

int main() {
    auto* mr_ptr =std::pmr::get_default_resource();
    auto supervisor = actor_zeta::spawn<supervisor_lite>(mr_ptr);

    int const actors = 5;

    std::vector<supervisor_lite::unique_future<void>> create_futures;
    create_futures.reserve(actors);
    for (auto i = actors; i > 0; --i) {
        create_futures.push_back(actor_zeta::send(supervisor.get(), actor_zeta::address_t::empty_address(), &supervisor_lite::create));
    }

    for (auto& future : create_futures) {
        std::move(future).get();
    }

    std::vector<worker_t::unique_future<std::size_t>> futures;
    futures.reserve(supervisor->worker_count());
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

    std::size_t total_size = 0;
    for (std::size_t i = 0; i < futures.size(); ++i) {
        std::size_t result = std::move(futures[i]).get();
        total_size += result;
    }

    std::cerr << "Total size from all workers: " << total_size << std::endl;

    return 0;
}
