#include <benchmark/benchmark.h>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/scheduler.hpp>
#include <actor-zeta/scheduler/policy/work_sharing.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>

std::atomic<int> ping_pong_counter{0};
std::atomic<bool> ping_pong_done{false};

// The round trip must close; if it does not, the benchmark is timing nothing.
// The fixture owns the actors, so discharging what they recorded is its job.
template<typename Actor>
inline void await_ping_pong(Actor* sender, Actor* target,
                            actor_zeta::scheduler::sharing_scheduler* sched) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ping_pong_done.load(std::memory_order_acquire)) {
        if (sender->take_partner_obligations() > 0) {
            sched->enqueue(target);
        }
        if (std::chrono::steady_clock::now() > deadline) {
            std::fprintf(stderr, "ping-pong did not complete: a needs_sched was dropped\n");
            std::abort();
        }
        std::this_thread::yield();
    }
}

template<typename... Args>
class ping_pong_actor final : public actor_zeta::basic_actor<ping_pong_actor<Args...>> {
    ping_pong_actor* partner_;
    std::atomic<std::size_t> partner_owed_{0};

public:
    explicit ping_pong_actor(std::pmr::memory_resource* res)
        : actor_zeta::basic_actor<ping_pong_actor<Args...>>(res)
        , partner_(nullptr) {
    }

    void set_partner(ping_pong_actor* p) { partner_ = p; }

    /// Claimed by the fixture, which owns the actors and therefore schedules them.
    std::size_t take_partner_obligations() {
        return partner_owed_.exchange(0, std::memory_order_acq_rel);
    }

    actor_zeta::unique_future<void> ping(Args...) {
        ++ping_pong_counter;
        if (partner_) {
            auto [needs_sched, future] = actor_zeta::send(partner_, &ping_pong_actor::pong, Args{}...);
            actor_zeta::detail::ignore_unused(future);
            // Dropping this strands the partner: the send took its turn out of the
            // mailbox, no job holds it, and every later send reports needs_sched ==
            // false. This actor has no scheduler, so it records and the owner claims.
            if (needs_sched) {
                partner_owed_.fetch_add(1, std::memory_order_release);
            }
        }
        co_return;
    }

    actor_zeta::unique_future<void> pong(Args...) {
        ++ping_pong_counter;
        ping_pong_done.store(true, std::memory_order_release);
        co_return;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &ping_pong_actor::ping,
        &ping_pong_actor::pong
    >;

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {

        switch (msg->command()) {
            case actor_zeta::msg_id<ping_pong_actor, &ping_pong_actor::ping>:
                co_await actor_zeta::dispatch(this, &ping_pong_actor::ping, msg);
                break;
            case actor_zeta::msg_id<ping_pong_actor, &ping_pong_actor::pong>:
                co_await actor_zeta::dispatch(this, &ping_pong_actor::pong, msg);
                break;
        }
    }
};

class PingPongFixture_0 : public benchmark::Fixture {
    using Actor = ping_pong_actor<>;

    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler_;
    std::unique_ptr<Actor, actor_zeta::pmr::deleter_t> actor0_;
    std::unique_ptr<Actor, actor_zeta::pmr::deleter_t> actor1_;
    std::pmr::memory_resource* resource_;

public:
    PingPongFixture_0() : actor0_(nullptr, actor_zeta::pmr::deleter_t(std::pmr::get_default_resource())),
                          actor1_(nullptr, actor_zeta::pmr::deleter_t(std::pmr::get_default_resource())) {}

    void SetUp(const benchmark::State&) override {
        resource_ =std::pmr::get_default_resource();
        scheduler_.reset(new actor_zeta::scheduler::scheduler_t<actor_zeta::scheduler::work_sharing>(resource_, 1, 1000));
        scheduler_->start();
        actor0_ = actor_zeta::spawn<Actor>(resource_);
        actor1_ = actor_zeta::spawn<Actor>(resource_);
        actor0_->set_partner(actor1_.get());
        actor1_->set_partner(actor0_.get());
        ping_pong_counter = 0;
    }

    void TearDown(const benchmark::State&) override {
        scheduler_->stop();
        actor0_.reset();
        actor1_.reset();
        scheduler_.reset();
    }

    void DoPingPong() {
        ping_pong_counter = 0;
        ping_pong_done.store(false, std::memory_order_release);
        auto [needs_sched, future] = actor_zeta::send(actor0_.get(), &Actor::ping);
        actor_zeta::detail::ignore_unused(future);
        if (needs_sched) {
            scheduler_->enqueue(actor0_.get());
        }
        // Measure the round trip, not a fixed sleep.
        await_ping_pong(actor0_.get(), actor1_.get(), scheduler_.get());
    }
};

class PingPongFixture_1 : public benchmark::Fixture {
    using Actor = ping_pong_actor<int64_t>;

    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler_;
    std::unique_ptr<Actor, actor_zeta::pmr::deleter_t> actor0_;
    std::unique_ptr<Actor, actor_zeta::pmr::deleter_t> actor1_;
    std::pmr::memory_resource* resource_;

public:
    PingPongFixture_1() : actor0_(nullptr, actor_zeta::pmr::deleter_t(std::pmr::get_default_resource())),
                          actor1_(nullptr, actor_zeta::pmr::deleter_t(std::pmr::get_default_resource())) {}

    void SetUp(const benchmark::State&) override {
        resource_ =std::pmr::get_default_resource();
        scheduler_.reset(new actor_zeta::scheduler::scheduler_t<actor_zeta::scheduler::work_sharing>(resource_, 1, 1000));
        scheduler_->start();
        actor0_ = actor_zeta::spawn<Actor>(resource_);
        actor1_ = actor_zeta::spawn<Actor>(resource_);
        actor0_->set_partner(actor1_.get());
        actor1_->set_partner(actor0_.get());
        ping_pong_counter = 0;
    }

    void TearDown(const benchmark::State&) override {
        scheduler_->stop();
        actor0_.reset();
        actor1_.reset();
        scheduler_.reset();
    }

    void DoPingPong() {
        ping_pong_counter = 0;
        ping_pong_done.store(false, std::memory_order_release);
        auto [needs_sched, future] = actor_zeta::send(actor0_.get(), &Actor::ping, int64_t{});
        actor_zeta::detail::ignore_unused(future);
        if (needs_sched) {
            scheduler_->enqueue(actor0_.get());
        }
        // Measure the round trip, not a fixed sleep.
        await_ping_pong(actor0_.get(), actor1_.get(), scheduler_.get());
    }
};

BENCHMARK_DEFINE_F(PingPongFixture_0, PingPong)(benchmark::State& st) {
    for (auto _ : st) {
        DoPingPong();
    }
}
BENCHMARK_REGISTER_F(PingPongFixture_0, PingPong);

BENCHMARK_DEFINE_F(PingPongFixture_1, PingPong)(benchmark::State& st) {
    for (auto _ : st) {
        DoPingPong();
    }
}
BENCHMARK_REGISTER_F(PingPongFixture_1, PingPong);

BENCHMARK_MAIN();