// Non-blocking delegation / proxy pattern.
//
// A router based on actor_mixin owns a pool of cooperative-actor workers and
// distributes incoming requests round-robin. Its enqueue_impl never calls
// behavior(): instead it re-stamps the message's command via message::set_command
// to the worker's method id, forwards the same message_ptr to the worker's
// enqueue_impl, and schedules the worker. The caller's future is filled by the
// worker through the message's type-erased result_slot_ (it travels with the
// message). The router itself does no co_await, holds no mutex, runs no pump —
// concurrent send() calls into the router are processed in parallel by the pool.

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

// =============================================================================
// Worker: real compute actor (async; cooperative_actor via basic_actor)
// =============================================================================
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

// =============================================================================
// Router: actor_mixin-based delegator. Its enqueue_impl forwards each message
// to a pool of worker_t via msg->set_command(...) and worker->enqueue_impl(...).
// =============================================================================
class router_t final : public actor::actor_mixin<router_t> {
public:
    // actor_mixin doesn't provide this alias (cooperative_actor does); the
    // dispatch / coroutine_traits machinery looks it up on the Actor type.
    template<typename T> using unique_future = actor_zeta::unique_future<T>;

    // The router's compute() exists ONLY so that:
    //   - msg_id<router_t, &router_t::compute> can be formed (and registered in
    //     dispatch_traits), and
    //   - send(router, &router_t::compute, ...) passes validate_method_for_send.
    // Its body never runs: enqueue_impl below intercepts every message and the
    // worker fills the caller's future. The signature must match worker_t::compute
    // (same unique_future<value_type>, same parameter types) because result_slot_
    // is type-erased (void*) and reinterpreted as shared_state<value_type>* by the
    // worker's dispatch — see plan, "несущее ограничение".
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

    // Non-blocking delegation: pick a worker, re-stamp the command, forward the
    // same message_ptr. The worker's dispatch fills the caller's future via the
    // inherited result_slot_. The router never waits.
    [[nodiscard]] std::pair<bool, detail::enqueue_result>
    enqueue_impl(mailbox::message_ptr msg) {
        auto& w = *workers_[rr_.fetch_add(1, std::memory_order_relaxed) % workers_.size()];
        msg->set_command(msg_id<worker_t, &worker_t::compute>);
        auto result = w.enqueue_impl(std::move(msg));
        if (result.first) {
            sched_->enqueue(&w);   // worker is a cooperative_actor: schedule on its scheduler
        }
        return {false, detail::enqueue_result::success};  // router itself never needs scheduling
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

    // Fire N requests at the router; each one lands on some worker via delegation.
    std::vector<unique_future<int>> futs;
    futs.reserve(N);
    for (int i = 0; i < N; ++i) {
        auto [needs_sched, f] = send(router.get(), &router_t::compute, i);
        (void) needs_sched;  // router is sync-style; its enqueue_impl scheduled the worker.
        futs.push_back(std::move(f));
    }

    // Drive each future to completion via run_until_complete with a yield pump:
    // workers run on `sched`'s threads (cross-thread), so the main thread cannot
    // pump them itself (Part 1 category C pattern).
    int sum = 0;
    for (auto& f : futs) {
        sum += run_until_complete(f, [] { std::this_thread::yield(); });
    }

    int expected = 0;
    for (int i = 0; i < N; ++i) { expected += i * 2; }

    std::cout << "delegation example: sum = " << sum
              << " (expected " << expected << ")" << std::endl;

    // CLAUDE.md shutdown order: stop the scheduler BEFORE the actors are destroyed.
    sched->stop();
    return sum == expected ? 0 : 1;
}