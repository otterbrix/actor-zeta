#include <benchmark/benchmark.h>
#include <actor-zeta.hpp>
#include <actor-zeta/dispatch.hpp>
#include <cstdint>

using namespace actor_zeta;

// =====================================================================
// OLD STYLE: Switch-based dispatch
// =====================================================================

class old_style_actor : public base::basic_actor<old_style_actor> {
public:
    // Declare methods first (needed by dispatch_traits)
    unique_future<void> method1(int x) {
        counter_ += x;
        return make_ready_future_void(resource());
    }
    unique_future<void> method2(int x) {
        counter_ += x * 2;
        return make_ready_future_void(resource());
    }
    unique_future<void> method3(int x) {
        counter_ += x * 3;
        return make_ready_future_void(resource());
    }
    unique_future<void> method4(int x) {
        counter_ += x * 4;
        return make_ready_future_void(resource());
    }
    unique_future<void> method5(int x) {
        counter_ += x * 5;
        return make_ready_future_void(resource());
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &old_style_actor::method1,
        &old_style_actor::method2,
        &old_style_actor::method3,
        &old_style_actor::method4,
        &old_style_actor::method5
    >;

    explicit old_style_actor(std::pmr::memory_resource* resource)
        : base::basic_actor<old_style_actor>(resource)
        , counter_(0) {}

    unique_future<void> behavior(mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == msg_id<old_style_actor, &old_style_actor::method1>) {
            dispatch(this, &old_style_actor::method1, msg);
        } else if (cmd == msg_id<old_style_actor, &old_style_actor::method2>) {
            dispatch(this, &old_style_actor::method2, msg);
        } else if (cmd == msg_id<old_style_actor, &old_style_actor::method3>) {
            dispatch(this, &old_style_actor::method3, msg);
        } else if (cmd == msg_id<old_style_actor, &old_style_actor::method4>) {
            dispatch(this, &old_style_actor::method4, msg);
        } else if (cmd == msg_id<old_style_actor, &old_style_actor::method5>) {
            dispatch(this, &old_style_actor::method5, msg);
        }
        return make_ready_future_void(resource());
    }

    int counter() const { return counter_; }

private:
    int counter_;
};

// =====================================================================
// Benchmark: Old style dispatch
// =====================================================================

static void BM_OldStyleDispatch(benchmark::State& state) {
    auto resource = std::pmr::get_default_resource();
    auto actor = spawn<old_style_actor>(resource);

    int method_id = static_cast<int>(state.range(0));

    for (auto _ : state) {
        switch (method_id) {
            case 0: {
                auto f = send(actor.get(), actor->address(), &old_style_actor::method1, 1);
                while (!f.available()) { actor->resume(1); }
                std::move(f).get();  // Wait for completion (returns void)
                break;
            }
            case 1: {
                auto f = send(actor.get(), actor->address(), &old_style_actor::method2, 2);
                while (!f.available()) { actor->resume(1); }
                std::move(f).get();  // Wait for completion (returns void)
                break;
            }
            case 2: {
                auto f = send(actor.get(), actor->address(), &old_style_actor::method3, 3);
                while (!f.available()) { actor->resume(1); }
                std::move(f).get();  // Wait for completion (returns void)
                break;
            }
            case 3: {
                auto f = send(actor.get(), actor->address(), &old_style_actor::method4, 4);
                while (!f.available()) { actor->resume(1); }
                std::move(f).get();  // Wait for completion (returns void)
                break;
            }
            case 4: {
                auto f = send(actor.get(), actor->address(), &old_style_actor::method5, 5);
                while (!f.available()) { actor->resume(1); }
                std::move(f).get();  // Wait for completion (returns void)
                break;
            }
        }
    }

    state.SetItemsProcessed(state.iterations());
    benchmark::DoNotOptimize(actor->counter());
}

BENCHMARK(BM_OldStyleDispatch)->DenseRange(0, 4)->Unit(benchmark::kNanosecond);

// =====================================================================
// Benchmark: Direct method call vs dispatch()
// =====================================================================

class coroutine_actor : public base::basic_actor<coroutine_actor> {
public:
    // Coroutine methods (no make_ready_future needed)
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
        : base::basic_actor<coroutine_actor>(resource) {}

    unique_future<void> behavior(mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == msg_id<coroutine_actor, &coroutine_actor::compute>) {
            dispatch(this, &coroutine_actor::compute, msg);
        } else if (cmd == msg_id<coroutine_actor, &coroutine_actor::noop>) {
            dispatch(this, &coroutine_actor::noop, msg);
        } else if (cmd == msg_id<coroutine_actor, &coroutine_actor::sum>) {
            dispatch(this, &coroutine_actor::sum, msg);
        } else if (cmd == msg_id<coroutine_actor, &coroutine_actor::sum3>) {
            dispatch(this, &coroutine_actor::sum3, msg);
        }
        co_return;
    }
};

// Direct method call (baseline - no dispatch overhead)
static void BM_DirectCall_Coroutine(benchmark::State& state) {
    auto resource = std::pmr::get_default_resource();
    auto actor = spawn<coroutine_actor>(resource);

    for (auto _ : state) {
        auto future = actor->compute(42);
        int result = std::move(future).get();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DirectCall_Coroutine)->Unit(benchmark::kNanosecond);

// dispatch() with 0 args
static void BM_Dispatch_0Args(benchmark::State& state) {
    auto resource = std::pmr::get_default_resource();
    auto actor = spawn<coroutine_actor>(resource);

    // Create message once, reuse
    auto msg = detail::make_message(
        resource,
        actor->address(),
        msg_id<coroutine_actor, &coroutine_actor::noop>
    );

    for (auto _ : state) {
        auto future = dispatch(actor.get(), &coroutine_actor::noop, msg.get());
        benchmark::DoNotOptimize(future);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Dispatch_0Args)->Unit(benchmark::kNanosecond);

// dispatch() with 1 arg
static void BM_Dispatch_1Arg(benchmark::State& state) {
    auto resource = std::pmr::get_default_resource();
    auto actor = spawn<coroutine_actor>(resource);

    for (auto _ : state) {
        auto msg = detail::make_message(
            resource,
            actor->address(),
            msg_id<coroutine_actor, &coroutine_actor::compute>,
            42
        );
        auto future = dispatch(actor.get(), &coroutine_actor::compute, msg.get());
        int result = std::move(future).get();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Dispatch_1Arg)->Unit(benchmark::kNanosecond);

// dispatch() with 2 args
static void BM_Dispatch_2Args(benchmark::State& state) {
    auto resource = std::pmr::get_default_resource();
    auto actor = spawn<coroutine_actor>(resource);

    for (auto _ : state) {
        auto msg = detail::make_message(
            resource,
            actor->address(),
            msg_id<coroutine_actor, &coroutine_actor::sum>,
            10, 20
        );
        auto future = dispatch(actor.get(), &coroutine_actor::sum, msg.get());
        int result = std::move(future).get();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Dispatch_2Args)->Unit(benchmark::kNanosecond);

// dispatch() with 3 args
static void BM_Dispatch_3Args(benchmark::State& state) {
    auto resource = std::pmr::get_default_resource();
    auto actor = spawn<coroutine_actor>(resource);

    for (auto _ : state) {
        auto msg = detail::make_message(
            resource,
            actor->address(),
            msg_id<coroutine_actor, &coroutine_actor::sum3>,
            10, 20, 30
        );
        auto future = dispatch(actor.get(), &coroutine_actor::sum3, msg.get());
        int result = std::move(future).get();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Dispatch_3Args)->Unit(benchmark::kNanosecond);

// Full send/dispatch/resume cycle for comparison
static void BM_FullCycle_Coroutine(benchmark::State& state) {
    auto resource = std::pmr::get_default_resource();
    auto actor = spawn<coroutine_actor>(resource);

    for (auto _ : state) {
        auto f = send(actor.get(), actor->address(), &coroutine_actor::compute, 42);
        while (!f.available()) { actor->resume(1); }
        int result = std::move(f).get();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FullCycle_Coroutine)->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();