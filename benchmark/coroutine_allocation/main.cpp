#include <benchmark/benchmark.h>
#include <actor-zeta.hpp>
#include <memory_resource>
#include <vector>

class BenchActor : public actor_zeta::basic_actor<BenchActor> {
public:
    using base_type = actor_zeta::basic_actor<BenchActor>;

    explicit BenchActor(std::pmr::memory_resource* res)
        : base_type(res)
        , call_count_(0) {}

    actor_zeta::unique_future<void> noop() {
        co_return;
    }

    actor_zeta::unique_future<int> compute(int x) {
        co_return x * 2;
    }

    actor_zeta::unique_future<std::string> format(int x) {
        co_return std::string("Result: ") + std::to_string(x);
    }

    actor_zeta::unique_future<void> increment() {
        ++call_count_;
        co_return;
    }

    int call_count() const { return call_count_; }

    void behavior(actor_zeta::mailbox::message*) {
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &BenchActor::noop,
        &BenchActor::compute,
        &BenchActor::format,
        &BenchActor::increment
    >;

private:
    int call_count_;
};

static void BM_CoroCreation_Void(benchmark::State& state) {
    auto* resource =std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<BenchActor>(resource);

    for (auto _ : state) {
        auto future = actor->noop();
        benchmark::DoNotOptimize(future);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CoroCreation_Void);

static void BM_CoroCreation_Int(benchmark::State& state) {
    auto* resource =std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<BenchActor>(resource);

    for (auto _ : state) {
        auto future = actor->compute(42);
        benchmark::DoNotOptimize(future);
        int result = std::move(future).get();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CoroCreation_Int);

static void BM_CoroCreation_String(benchmark::State& state) {
    auto* resource =std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<BenchActor>(resource);

    for (auto _ : state) {
        auto future = actor->format(42);
        benchmark::DoNotOptimize(future);
        std::string result = std::move(future).get();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CoroCreation_String);

static void BM_CoroSequence(benchmark::State& state) {
    auto* resource =std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<BenchActor>(resource);
    const int count = state.range(0);

    for (auto _ : state) {
        for (int i = 0; i < count; ++i) {
            auto future = actor->compute(i);
            int result = std::move(future).get();
            benchmark::DoNotOptimize(result);
        }
    }

    state.SetItemsProcessed(state.iterations() * count);
}
BENCHMARK(BM_CoroSequence)->Range(1, 1024);

static void BM_RawAlloc_New(benchmark::State& state) {
    const size_t size = state.range(0);

    for (auto _ : state) {
        void* p = ::operator new(size);
        benchmark::DoNotOptimize(p);
        ::operator delete(p);
    }

    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * size);
}
BENCHMARK(BM_RawAlloc_New)->Range(64, 1024);

static void BM_RawAlloc_PMR(benchmark::State& state) {
    auto* resource =std::pmr::get_default_resource();
    const size_t size = state.range(0);

    for (auto _ : state) {
        void* p = resource->allocate(size, alignof(std::max_align_t));
        benchmark::DoNotOptimize(p);
        resource->deallocate(p, size, alignof(std::max_align_t));
    }

    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * size);
}
BENCHMARK(BM_RawAlloc_PMR)->Range(64, 1024);

static void BM_FutureState_Int(benchmark::State& state) {
    auto* resource =std::pmr::get_default_resource();

    for (auto _ : state) {
        actor_zeta::promise<int> p(resource);
        p.set_value(42);
        auto future = p.get_future();
        benchmark::DoNotOptimize(future);
        int result = std::move(future).get();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FutureState_Int);

static void BM_FutureState_Void(benchmark::State& state) {
    auto* resource =std::pmr::get_default_resource();

    for (auto _ : state) {
        actor_zeta::promise<void> p(resource);
        p.set_value();
        auto future = p.get_future();
        benchmark::DoNotOptimize(future);
        std::move(future).get();
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FutureState_Void);

static void BM_SimpleCoroutine_Int(benchmark::State& state) {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<BenchActor>(resource);

    for (auto _ : state) {
        auto future = actor->compute(42);
        benchmark::DoNotOptimize(future);
        int result = std::move(future).get();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SimpleCoroutine_Int);

static void BM_SimpleCoroutine_Void(benchmark::State& state) {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<BenchActor>(resource);

    for (auto _ : state) {
        auto future = actor->noop();
        benchmark::DoNotOptimize(future);
        std::move(future).get();
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SimpleCoroutine_Void);

static void BM_CoroOverhead_Estimate(benchmark::State& state) {
    auto* resource = std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<BenchActor>(resource);

    // Warm up
    for (int i = 0; i < 100; ++i) {
        auto f1 = actor->compute(i);
        std::move(f1).get();
    }

    for (auto _ : state) {
        // Coroutine path
        auto future = actor->compute(42);
        int result = std::move(future).get();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
    state.SetLabel("coroutine frame + future_state");
}
BENCHMARK(BM_CoroOverhead_Estimate);

static void BM_Coro_MonotonicBuffer(benchmark::State& state) {
    constexpr size_t buffer_size = 1024 * 1024;  // 1MB
    std::vector<std::byte> buffer(buffer_size);
    std::pmr::monotonic_buffer_resource mono_resource(
        buffer.data(), buffer_size, std::pmr::null_memory_resource()
    );

    auto actor = actor_zeta::spawn<BenchActor>(&mono_resource);

    for (auto _ : state) {
        state.PauseTiming();
        mono_resource.release();
        actor = actor_zeta::spawn<BenchActor>(&mono_resource);
        state.ResumeTiming();

        for (int i = 0; i < 1000; ++i) {
            auto future = actor->compute(42);
            int result = std::move(future).get();
            benchmark::DoNotOptimize(result);
        }
    }

    state.SetItemsProcessed(state.iterations() * 1000);
    state.SetLabel("monotonic_buffer");
}
BENCHMARK(BM_Coro_MonotonicBuffer);

static void BM_Coro_UnsyncPool(benchmark::State& state) {
    std::pmr::unsynchronized_pool_resource pool_resource;
    auto actor = actor_zeta::spawn<BenchActor>(&pool_resource);

    for (auto _ : state) {
        auto future = actor->compute(42);
        int result = std::move(future).get();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
    state.SetLabel("unsync_pool");
}
BENCHMARK(BM_Coro_UnsyncPool);

static void BM_Coro_SyncPool(benchmark::State& state) {
    std::pmr::synchronized_pool_resource pool_resource;
    auto actor = actor_zeta::spawn<BenchActor>(&pool_resource);

    for (auto _ : state) {
        auto future = actor->compute(42);
        int result = std::move(future).get();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
    state.SetLabel("sync_pool");
}
BENCHMARK(BM_Coro_SyncPool);

static void BM_Coro_NewDelete(benchmark::State& state) {
    auto* resource = std::pmr::new_delete_resource();
    auto actor = actor_zeta::spawn<BenchActor>(resource);

    for (auto _ : state) {
        auto future = actor->compute(42);
        int result = std::move(future).get();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
    state.SetLabel("new_delete");
}
BENCHMARK(BM_Coro_NewDelete);

static void BM_RawAlloc_Monotonic(benchmark::State& state) {
    constexpr size_t buffer_size = 1024 * 1024;
    std::vector<std::byte> buffer(buffer_size);
    std::pmr::monotonic_buffer_resource mono_resource(
        buffer.data(), buffer_size, std::pmr::null_memory_resource()
    );
    const size_t size = 128;

    for (auto _ : state) {
        state.PauseTiming();
        mono_resource.release();
        state.ResumeTiming();

        for (int i = 0; i < 1000; ++i) {
            void* p = mono_resource.allocate(size, alignof(std::max_align_t));
            benchmark::DoNotOptimize(p);
        }
    }

    state.SetItemsProcessed(state.iterations() * 1000);
    state.SetLabel("monotonic (no dealloc)");
}
BENCHMARK(BM_RawAlloc_Monotonic);

static void BM_RawAlloc_UnsyncPool(benchmark::State& state) {
    std::pmr::unsynchronized_pool_resource pool_resource;
    const size_t size = 128;

    for (auto _ : state) {
        void* p = pool_resource.allocate(size, alignof(std::max_align_t));
        benchmark::DoNotOptimize(p);
        pool_resource.deallocate(p, size, alignof(std::max_align_t));
    }

    state.SetItemsProcessed(state.iterations());
    state.SetLabel("unsync_pool");
}
BENCHMARK(BM_RawAlloc_UnsyncPool);

static void BM_RawAlloc_SyncPool(benchmark::State& state) {
    std::pmr::synchronized_pool_resource pool_resource;
    const size_t size = 128;

    for (auto _ : state) {
        void* p = pool_resource.allocate(size, alignof(std::max_align_t));
        benchmark::DoNotOptimize(p);
        pool_resource.deallocate(p, size, alignof(std::max_align_t));
    }

    state.SetItemsProcessed(state.iterations());
    state.SetLabel("sync_pool");
}
BENCHMARK(BM_RawAlloc_SyncPool);

BENCHMARK_MAIN();