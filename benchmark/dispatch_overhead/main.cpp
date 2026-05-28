#include <benchmark/benchmark.h>
#include <actor-zeta.hpp>
#include <cstdint>

using namespace actor_zeta;

class old_style_actor : public basic_actor<old_style_actor> {
public:
    unique_future<void> method1(int x) {
        counter_ += x;
        co_return;
    }
    unique_future<void> method2(int x) {
        counter_ += x * 2;
        co_return;
    }
    unique_future<void> method3(int x) {
        counter_ += x * 3;
        co_return;
    }
    unique_future<void> method4(int x) {
        counter_ += x * 4;
        co_return;
    }
    unique_future<void> method5(int x) {
        counter_ += x * 5;
        co_return;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &old_style_actor::method1,
        &old_style_actor::method2,
        &old_style_actor::method3,
        &old_style_actor::method4,
        &old_style_actor::method5
    >;

    explicit old_style_actor(std::pmr::memory_resource* resource)
        : basic_actor<old_style_actor>(resource)
        , counter_(0) {}

    behavior_t behavior(mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == msg_id<old_style_actor, &old_style_actor::method1>) {
            co_await dispatch(this, &old_style_actor::method1, msg);
        } else if (cmd == msg_id<old_style_actor, &old_style_actor::method2>) {
            co_await dispatch(this, &old_style_actor::method2, msg);
        } else if (cmd == msg_id<old_style_actor, &old_style_actor::method3>) {
            co_await dispatch(this, &old_style_actor::method3, msg);
        } else if (cmd == msg_id<old_style_actor, &old_style_actor::method4>) {
            co_await dispatch(this, &old_style_actor::method4, msg);
        } else if (cmd == msg_id<old_style_actor, &old_style_actor::method5>) {
            co_await dispatch(this, &old_style_actor::method5, msg);
        }
    }

    int counter() const { return counter_; }

private:
    int counter_;
};

static void BM_OldStyleDispatch(benchmark::State& state) {
    auto resource = std::pmr::get_default_resource();
    auto actor = spawn<old_style_actor>(resource);

    int method_id = static_cast<int>(state.range(0));

    for (auto _ : state) {
        switch (method_id) {
            case 0: {
                auto [needs_sched, f] = send(actor.get(), &old_style_actor::method1, 1);
                run_until_complete(f, [&] { actor->resume(1); });
                break;
            }
            case 1: {
                auto [needs_sched, f] = send(actor.get(), &old_style_actor::method2, 2);
                run_until_complete(f, [&] { actor->resume(1); });
                break;
            }
            case 2: {
                auto [needs_sched, f] = send(actor.get(), &old_style_actor::method3, 3);
                run_until_complete(f, [&] { actor->resume(1); });
                break;
            }
            case 3: {
                auto [needs_sched, f] = send(actor.get(), &old_style_actor::method4, 4);
                run_until_complete(f, [&] { actor->resume(1); });
                break;
            }
            case 4: {
                auto [needs_sched, f] = send(actor.get(), &old_style_actor::method5, 5);
                run_until_complete(f, [&] { actor->resume(1); });
                break;
            }
        }
    }

    state.SetItemsProcessed(state.iterations());
    benchmark::DoNotOptimize(actor->counter());
}

BENCHMARK(BM_OldStyleDispatch)->DenseRange(0, 4)->Unit(benchmark::kNanosecond);

class coroutine_actor : public basic_actor<coroutine_actor> {
public:
    unique_future<int> compute(int x) {
        co_return x * 2;
    }

    unique_future<void> noop() {
        co_return;
    }

    unique_future<int> sum(int a, int b) {
        co_return a + b;
    }

    unique_future<int> sum3(int a, int b, int c) {
        co_return a + b + c;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &coroutine_actor::compute,
        &coroutine_actor::noop,
        &coroutine_actor::sum,
        &coroutine_actor::sum3
    >;

    explicit coroutine_actor(std::pmr::memory_resource* resource)
        : basic_actor<coroutine_actor>(resource) {}

    behavior_t behavior(mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == msg_id<coroutine_actor, &coroutine_actor::compute>) {
            co_await dispatch(this, &coroutine_actor::compute, msg);
        } else if (cmd == msg_id<coroutine_actor, &coroutine_actor::noop>) {
            co_await dispatch(this, &coroutine_actor::noop, msg);
        } else if (cmd == msg_id<coroutine_actor, &coroutine_actor::sum>) {
            co_await dispatch(this, &coroutine_actor::sum, msg);
        } else if (cmd == msg_id<coroutine_actor, &coroutine_actor::sum3>) {
            co_await dispatch(this, &coroutine_actor::sum3, msg);
        }
    }
};

static void BM_DirectCall_Coroutine(benchmark::State& state) {
    auto resource = std::pmr::get_default_resource();
    auto actor = spawn<coroutine_actor>(resource);

    for (auto _ : state) {
        auto future = actor->compute(42);
        int result = std::move(future).take_ready();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DirectCall_Coroutine)->Unit(benchmark::kNanosecond);

static void BM_Dispatch_0Args(benchmark::State& state) {
    auto resource = std::pmr::get_default_resource();
    auto actor = spawn<coroutine_actor>(resource);

    // Create message once, reuse
    auto [msg, future_unused] = detail::make_message(resource,
        msg_id<coroutine_actor, &coroutine_actor::noop>);

    for (auto _ : state) {
        auto future = dispatch(actor.get(), &coroutine_actor::noop, msg.get());
        benchmark::DoNotOptimize(future);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Dispatch_0Args)->Unit(benchmark::kNanosecond);

static void BM_FullCycle_1Arg(benchmark::State& state) {
    auto resource = std::pmr::get_default_resource();
    auto actor = spawn<coroutine_actor>(resource);

    for (auto _ : state) {
        auto [needs_sched, f] = send(actor.get(), &coroutine_actor::compute, 42);
        int result = run_until_complete(f, [&] { actor->resume(1); });
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FullCycle_1Arg)->Unit(benchmark::kNanosecond);

static void BM_FullCycle_2Args(benchmark::State& state) {
    auto resource = std::pmr::get_default_resource();
    auto actor = spawn<coroutine_actor>(resource);

    for (auto _ : state) {
        auto [needs_sched, f] = send(actor.get(), &coroutine_actor::sum, 10, 20);
        int result = run_until_complete(f, [&] { actor->resume(1); });
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FullCycle_2Args)->Unit(benchmark::kNanosecond);

static void BM_FullCycle_3Args(benchmark::State& state) {
    auto resource = std::pmr::get_default_resource();
    auto actor = spawn<coroutine_actor>(resource);

    for (auto _ : state) {
        auto [needs_sched, f] = send(actor.get(), &coroutine_actor::sum3, 10, 20, 30);
        int result = run_until_complete(f, [&] { actor->resume(1); });
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FullCycle_3Args)->Unit(benchmark::kNanosecond);

static void BM_FullCycle_Coroutine(benchmark::State& state) {
    auto resource = std::pmr::get_default_resource();
    auto actor = spawn<coroutine_actor>(resource);

    for (auto _ : state) {
        auto [needs_sched, f] = send(actor.get(), &coroutine_actor::compute, 42);
        int result = run_until_complete(f, [&] { actor->resume(1); });
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FullCycle_Coroutine)->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();