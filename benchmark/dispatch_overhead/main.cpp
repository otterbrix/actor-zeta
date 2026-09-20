#include <benchmark/benchmark.h>
#include <actor-zeta.hpp>
#include <cstdint>
#include <cstdlib>

using namespace actor_zeta;

// dispatch_overhead exists to price the dispatch path itself, so its driver has to stay
// on the benchmark thread: handing these actors to scheduler::sharing_scheduler would
// replace the number under test with a condition-variable wakeup and a cross-thread
// spin. This is that driver -- a one-actor, same-thread run queue. It exists so the
// resume verdict is discharged rather than dropped: `resume` means "put me back in a
// run queue", and here the run queue is this loop.
class same_thread_scheduler {
public:
    explicit same_thread_scheduler(size_t max_throughput) noexcept
        : max_throughput_(max_throughput) {}

    template<typename Actor>
    void enqueue(Actor* actor) {
        while (actor->resume(max_throughput_).result == scheduler::resume_result::resume) {
        }
    }

private:
    size_t max_throughput_;
};

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
    same_thread_scheduler sched(1);

    int method_id = static_cast<int>(state.range(0));

    // Precondition, established ONCE outside the measured region: one drive after one
    // send makes the future ready. It holds per actor+driver, not per method, so
    // probing method1 covers every case of the switch below. See BM_FullCycle_1Arg for
    // why take_ready() must not be reached on a future nothing has checked.
    {
        auto [probe_needs_sched, probe_future] = send(actor.get(), &old_style_actor::method1, 1);
        sched.enqueue(actor.get());
        if (!probe_future.is_ready() || probe_future.failed()) {
            std::abort();
        }
        std::move(probe_future).take_ready();
    }

    for (auto _ : state) {
        switch (method_id) {
            case 0: {
                auto [needs_sched, f] = send(actor.get(), &old_style_actor::method1, 1);
                sched.enqueue(actor.get());   // unconditional: this IS the dispatch under measurement
                std::move(f).take_ready();
                break;
            }
            case 1: {
                auto [needs_sched, f] = send(actor.get(), &old_style_actor::method2, 2);
                sched.enqueue(actor.get());
                std::move(f).take_ready();
                break;
            }
            case 2: {
                auto [needs_sched, f] = send(actor.get(), &old_style_actor::method3, 3);
                sched.enqueue(actor.get());
                std::move(f).take_ready();
                break;
            }
            case 3: {
                auto [needs_sched, f] = send(actor.get(), &old_style_actor::method4, 4);
                sched.enqueue(actor.get());
                std::move(f).take_ready();
                break;
            }
            case 4: {
                auto [needs_sched, f] = send(actor.get(), &old_style_actor::method5, 5);
                sched.enqueue(actor.get());
                std::move(f).take_ready();
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
        if (!future.is_ready() || future.failed()) {
            state.SkipWithError("drive left the future unready; timings would be meaningless");
            break;
        }
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
    same_thread_scheduler sched(1);

    // Precondition, established ONCE outside the measured region: one drive after one
    // send makes the future ready. take_ready() only ASSERTS readiness, and asserts
    // vanish under NDEBUG -- which benchmarks are built with -- so an unchecked
    // violation reads unset storage and reports a plausible wrong number. The timed
    // loop still gates on is_ready()/failed(): one predictable branch costs far less
    // than publishing a fabricated timing.
    {
        auto [probe_needs_sched, probe_future] = send(actor.get(), &coroutine_actor::compute, 42);
        sched.enqueue(actor.get());
        if (!probe_future.is_ready() || probe_future.failed()) {
            std::abort();
        }
        benchmark::DoNotOptimize(std::move(probe_future).take_ready());
    }

    for (auto _ : state) {
        auto [needs_sched, f] = send(actor.get(), &coroutine_actor::compute, 42);
        sched.enqueue(actor.get());   // unconditional: this IS the dispatch under measurement
        if (!f.is_ready() || f.failed()) {
            state.SkipWithError("drive left the future unready; timings would be meaningless");
            break;
        }
        int result = std::move(f).take_ready();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FullCycle_1Arg)->Unit(benchmark::kNanosecond);

static void BM_FullCycle_2Args(benchmark::State& state) {
    auto resource = std::pmr::get_default_resource();
    auto actor = spawn<coroutine_actor>(resource);
    same_thread_scheduler sched(1);

    // Precondition, established ONCE outside the measured region, as in
    // BM_FullCycle_1Arg: one drive after one send makes the future ready.
    {
        auto [probe_needs_sched, probe_future] = send(actor.get(), &coroutine_actor::sum, 10, 20);
        sched.enqueue(actor.get());
        if (!probe_future.is_ready() || probe_future.failed()) {
            std::abort();
        }
        benchmark::DoNotOptimize(std::move(probe_future).take_ready());
    }

    for (auto _ : state) {
        auto [needs_sched, f] = send(actor.get(), &coroutine_actor::sum, 10, 20);
        sched.enqueue(actor.get());   // unconditional: this IS the dispatch under measurement
        if (!f.is_ready() || f.failed()) {
            state.SkipWithError("drive left the future unready; timings would be meaningless");
            break;
        }
        int result = std::move(f).take_ready();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FullCycle_2Args)->Unit(benchmark::kNanosecond);

static void BM_FullCycle_3Args(benchmark::State& state) {
    auto resource = std::pmr::get_default_resource();
    auto actor = spawn<coroutine_actor>(resource);
    same_thread_scheduler sched(1);

    // Precondition, established ONCE outside the measured region, as in
    // BM_FullCycle_1Arg: one drive after one send makes the future ready.
    {
        auto [probe_needs_sched, probe_future] = send(actor.get(), &coroutine_actor::sum3, 10, 20, 30);
        sched.enqueue(actor.get());
        if (!probe_future.is_ready() || probe_future.failed()) {
            std::abort();
        }
        benchmark::DoNotOptimize(std::move(probe_future).take_ready());
    }

    for (auto _ : state) {
        auto [needs_sched, f] = send(actor.get(), &coroutine_actor::sum3, 10, 20, 30);
        sched.enqueue(actor.get());   // unconditional: this IS the dispatch under measurement
        if (!f.is_ready() || f.failed()) {
            state.SkipWithError("drive left the future unready; timings would be meaningless");
            break;
        }
        int result = std::move(f).take_ready();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FullCycle_3Args)->Unit(benchmark::kNanosecond);

static void BM_FullCycle_Coroutine(benchmark::State& state) {
    auto resource = std::pmr::get_default_resource();
    auto actor = spawn<coroutine_actor>(resource);
    same_thread_scheduler sched(1);

    // Precondition, established ONCE outside the measured region, as in
    // BM_FullCycle_1Arg: one drive after one send makes the future ready.
    {
        auto [probe_needs_sched, probe_future] = send(actor.get(), &coroutine_actor::compute, 42);
        sched.enqueue(actor.get());
        if (!probe_future.is_ready() || probe_future.failed()) {
            std::abort();
        }
        benchmark::DoNotOptimize(std::move(probe_future).take_ready());
    }

    for (auto _ : state) {
        auto [needs_sched, f] = send(actor.get(), &coroutine_actor::compute, 42);
        sched.enqueue(actor.get());   // unconditional: this IS the dispatch under measurement
        if (!f.is_ready() || f.failed()) {
            state.SkipWithError("drive left the future unready; timings would be meaningless");
            break;
        }
        int result = std::move(f).take_ready();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FullCycle_Coroutine)->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();