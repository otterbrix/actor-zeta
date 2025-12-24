#include <benchmark/benchmark.h>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/scheduler.hpp>
#include <actor-zeta/scheduler/policy/work_sharing.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <thread>
#include <chrono>

std::atomic<int> ping_pong_counter{0};
std::atomic<bool> ping_pong_done{false};

template<typename... Args>
class ping_pong_actor final : public actor_zeta::basic_actor<ping_pong_actor<Args...>> {
    ping_pong_actor* partner_;

public:
    explicit ping_pong_actor(std::pmr::memory_resource* res)
        : actor_zeta::basic_actor<ping_pong_actor<Args...>>(res)
        , partner_(nullptr) {
    }

    void set_partner(ping_pong_actor* p) { partner_ = p; }

    actor_zeta::unique_future<void> ping(Args...) {
        ++ping_pong_counter;
        if (partner_) {
            actor_zeta::send(partner_, this->address(), &ping_pong_actor::pong, Args{}...);
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

    void behavior(actor_zeta::mailbox::message* msg) {

        switch (msg->command()) {
            case actor_zeta::msg_id<ping_pong_actor, &ping_pong_actor::ping>:
                actor_zeta::dispatch(this, &ping_pong_actor::ping, msg);
                break;
            case actor_zeta::msg_id<ping_pong_actor, &ping_pong_actor::pong>:
                actor_zeta::dispatch(this, &ping_pong_actor::pong, msg);
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
        scheduler_.reset(new actor_zeta::scheduler::scheduler_t<actor_zeta::scheduler::work_sharing>(1, 1000));
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
        actor_zeta::send(actor0_.get(), actor0_->address(), &Actor::ping);
        scheduler_->enqueue(actor0_.get());
        std::this_thread::sleep_for(std::chrono::microseconds(100));
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
        scheduler_.reset(new actor_zeta::scheduler::scheduler_t<actor_zeta::scheduler::work_sharing>(1, 1000));
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
        actor_zeta::send(actor0_.get(), actor0_->address(), &Actor::ping, int64_t{});
        scheduler_->enqueue(actor0_.get());
        std::this_thread::sleep_for(std::chrono::microseconds(100));
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