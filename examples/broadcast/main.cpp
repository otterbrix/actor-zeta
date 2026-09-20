#include <cassert>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include <actor-zeta.hpp>

// Wait for a future completed elsewhere -- by a scheduler's worker threads, or inline
// by a synchronous actor_mixin. Nothing is pumped here: this is a bounded spin.
//
// is_ready() is NOT a value gate: it is the promise_released bit, which a promise that
// dies without a value sets too, and take_ready() only ASSERTS that a value is present
// -- an assert that is gone in the Release builds examples ship as. Hence failed(). The
// bound turns a producer that never completes into a visible error, not a silent hang.
template<typename T>
T await_from_scheduler(actor_zeta::unique_future<T>& future) {
    constexpr int kSpinCap = 10'000'000;
    for (int i = 0; i < kSpinCap && !future.is_ready(); ++i) {
        std::this_thread::yield();
    }
    if (!future.is_ready() || future.failed()) {
        std::cerr << "await_from_scheduler: future did not complete with a value\n";
        std::abort();
    }
    return std::move(future).take_ready();
}


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

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        switch (cmd) {
            case actor_zeta::msg_id<worker_t, &worker_t::download_with_result>:
                co_await actor_zeta::dispatch(this, &worker_t::download_with_result, msg);
                break;
            case actor_zeta::msg_id<worker_t, &worker_t::work_data_with_result>:
                co_await actor_zeta::dispatch(this, &worker_t::work_data_with_result, msg);
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

    [[nodiscard]] std::pair<bool, actor_zeta::detail::enqueue_result>
    enqueue_impl(actor_zeta::mailbox::message_ptr msg) {
        behavior(msg.get());
        return {false, actor_zeta::detail::enqueue_result::success};
    }

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

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<supervisor_lite, &supervisor_lite::create>) {
            co_await actor_zeta::dispatch(this, &supervisor_lite::create, msg);
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

private:
    std::size_t size_actor() noexcept {
        return size_actors_.load();
    }

    std::pmr::memory_resource* resource_;
    actor_zeta::sharing_scheduler* e_;
    std::vector<std::unique_ptr<worker_t, actor_zeta::pmr::deleter_t>> actors_;
    std::atomic<std::size_t> size_actors_{0};
    std::mutex mutex_;
};

int main() {
    auto* mr_ptr =std::pmr::get_default_resource();
    auto supervisor = actor_zeta::spawn<supervisor_lite>(mr_ptr);

    int const actors = 5;

    // The supervisor (an actor_mixin) processes create requests synchronously; the worker
    // download results below are produced on its internal scheduler (e_), so both are
    // collected by the same poll. The supervisor owns and stops e_ in its destructor, so
    // the actors and their scheduler outlive this collection.
    for (auto i = actors; i > 0; --i) {
        auto sent = actor_zeta::send(supervisor.get(), &supervisor_lite::create);
        // A synchronous actor_mixin runs the handler inside send(): nothing to schedule.
        assert(!sent.first);
        await_from_scheduler(sent.second);
    }

    std::size_t total_size = 0;
    for (std::size_t i = 0; i < supervisor->worker_count(); ++i) {
        auto* worker = supervisor->get_worker(i);
        if (worker) {
            auto [needs_sched, future] = actor_zeta::send(
                worker,
                &worker_t::download_with_result,
                std::string("test_data_" + std::to_string(i)),
                std::string("user"),
                std::string("pass")
            );

            if (needs_sched) {
                supervisor->schedule_worker(i);
            }
            // Worker runs on the supervisor's internal scheduler (cross-thread).
            total_size += await_from_scheduler(future);
        }
    }

    std::cerr << "Total size from all workers: " << total_size << std::endl;

    return 0;
}
