// Non-blocking delegation pattern: an actor_mixin router restamps the message's
// command via message::set_command and forwards it to one of a pool of
// cooperative-actor workers. The caller's future is filled by the worker through
// the message's type-erased result_slot; the router itself never waits.

#include <atomic>
#include <cstddef>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <thread>
#include <vector>

#include <actor-zeta.hpp>
#include <actor-zeta/actor/dispatch.hpp>

using namespace actor_zeta;

class worker_t final : public basic_actor<worker_t> {
public:
    explicit worker_t(std::pmr::memory_resource* res)
        : basic_actor<worker_t>(res) {}

    unique_future<int> compute(int x) { co_return x * 2; }

    using dispatch_traits = actor_zeta::dispatch_traits<&worker_t::compute>;

    behavior_t behavior(mailbox::message* msg) {
        if (msg->command() == msg_id<worker_t, &worker_t::compute>) {
            co_await dispatch(this, &worker_t::compute, msg);
        }
    }
};

class router_t final : public actor::actor_mixin<router_t> {
public:
    template<typename T> using unique_future = actor_zeta::unique_future<T>;

    // The body never runs: enqueue_impl below intercepts every message. The
    // signature must match worker_t::compute (same value_type + parameter types)
    // because result_slot_ is reinterpreted as shared_state<value_type>*.
    unique_future<int> compute(int) { co_return 0; }

    using dispatch_traits = actor_zeta::dispatch_traits<&router_t::compute>;

    router_t(std::pmr::memory_resource* res,
             scheduler::sharing_scheduler* sched,
             std::size_t pool_size)
        : res_(res), sched_(sched) {
        workers_.reserve(pool_size);
        for (std::size_t i = 0; i < pool_size; ++i) {
            workers_.emplace_back(spawn<worker_t>(res));
        }
    }

    std::pmr::memory_resource* resource() const noexcept { return res_; }

    [[nodiscard]] std::pair<bool, detail::enqueue_result>
    enqueue_impl(mailbox::message_ptr msg) {
        auto& w = *workers_[rr_.fetch_add(1, std::memory_order_relaxed) % workers_.size()];
        msg->set_command(msg_id<worker_t, &worker_t::compute>);
        auto result = w.enqueue_impl(std::move(msg));
        if (result.first) {
            sched_->enqueue(&w);
        }
        return {false, detail::enqueue_result::success};
    }

private:
    std::pmr::memory_resource* res_;
    std::vector<std::unique_ptr<worker_t, pmr::deleter_t>> workers_;
    scheduler::sharing_scheduler* sched_;
    std::atomic<std::size_t> rr_{0};
};

int main() {
    auto* res = std::pmr::get_default_resource();

    auto sched = std::make_unique<scheduler::sharing_scheduler>(4, 1000);
    sched->start();

    constexpr std::size_t POOL = 4;
    constexpr int N = 16;

    auto router = spawn<router_t>(res, sched.get(), POOL);

    std::vector<unique_future<int>> futs;
    futs.reserve(N);
    for (int i = 0; i < N; ++i) {
        auto [needs_sched, f] = send(router.get(), &router_t::compute, i);
        (void) needs_sched;
        futs.push_back(std::move(f));
    }

    int sum = 0;
    for (auto& f : futs) {
        sum += run_until_complete(f, [] { std::this_thread::yield(); });
    }

    int expected = 0;
    for (int i = 0; i < N; ++i) { expected += i * 2; }

    std::cout << "delegation example: sum = " << sum
              << " (expected " << expected << ")" << std::endl;

    sched->stop();   // before actors are destroyed (CLAUDE.md shutdown order)
    return sum == expected ? 0 : 1;
}