/**
 * @file main.cpp
 * @brief Benchmark for coroutine frame allocation performance
 *
 * This benchmark measures the performance of coroutine frame allocation
 * with different memory resources to establish a baseline before
 * implementing custom allocator support in promise_type.
 *
 * Benchmarks:
 * 1. BM_CoroCreation_* - Coroutine creation with different allocators
 * 2. BM_RawAllocation - Raw allocation for comparison
 * 3. BM_FutureStateOnly - future_state allocation only (no coroutine)
 */

#include <benchmark/benchmark.h>
#include <actor-zeta.hpp>
#include <memory_resource>
#include <vector>

// =============================================================================
// Test Actor for benchmarks
// =============================================================================

class BenchActor : public actor_zeta::basic_actor<BenchActor> {
public:
    using base_type = actor_zeta::basic_actor<BenchActor>;

    explicit BenchActor(std::pmr::memory_resource* res)
        : base_type(res)
        , call_count_(0) {}

    // Simple void coroutine - minimal overhead
    actor_zeta::unique_future<void> noop() {
        co_return;
    }

    // Coroutine with int result
    actor_zeta::unique_future<int> compute(int x) {
        co_return x * 2;
    }

    // Coroutine with string result (heap allocation inside)
    actor_zeta::unique_future<std::string> format(int x) {
        co_return std::string("Result: ") + std::to_string(x);
    }

    // Counter for verification
    actor_zeta::unique_future<void> increment() {
        ++call_count_;
        co_return;
    }

    int call_count() const { return call_count_; }

    actor_zeta::unique_future<void> behavior(actor_zeta::mailbox::message*) {
        co_return;
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

// =============================================================================
// Benchmark: Coroutine Creation Baseline
// =============================================================================

static void BM_CoroCreation_Void(benchmark::State& state) {
    auto* resource =std::pmr::get_default_resource();
    auto actor = actor_zeta::spawn<BenchActor>(resource);

    for (auto _ : state) {
        auto future = actor->noop();
        benchmark::DoNotOptimize(future);
        // Future destroyed here - coroutine cleanup
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

// =============================================================================
// Benchmark: Multiple coroutines in sequence
// =============================================================================

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

// =============================================================================
// Benchmark: Raw allocation comparison
// =============================================================================

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

// =============================================================================
// Benchmark: future_state allocation only (no coroutine frame)
// =============================================================================

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

// =============================================================================
// Benchmark: make_ready_future (synchronous path)
// =============================================================================

static void BM_MakeReadyFuture_Int(benchmark::State& state) {
    auto* resource =std::pmr::get_default_resource();

    for (auto _ : state) {
        auto future = actor_zeta::make_ready_future<int>(resource, 42);
        benchmark::DoNotOptimize(future);
        int result = std::move(future).get();
        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MakeReadyFuture_Int);

static void BM_MakeReadyFuture_Void(benchmark::State& state) {
    auto* resource = std::pmr::get_default_resource();

    for (auto _ : state) {
        auto future = actor_zeta::make_ready_future_void(resource);
        benchmark::DoNotOptimize(future);
        std::move(future).get();
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MakeReadyFuture_Void);

// =============================================================================
// Benchmark: Estimate coroutine frame overhead
// =============================================================================

// This measures the DIFFERENCE between coroutine and make_ready_future
// to estimate pure coroutine frame allocation cost
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

// =============================================================================
// Benchmark: Different allocators comparison
// =============================================================================

// Monotonic buffer - fastest (bump allocator, no individual deallocation)
static void BM_Coro_MonotonicBuffer(benchmark::State& state) {
    constexpr size_t buffer_size = 1024 * 1024;  // 1MB
    std::vector<std::byte> buffer(buffer_size);
    std::pmr::monotonic_buffer_resource mono_resource(
        buffer.data(), buffer_size, std::pmr::null_memory_resource()
    );

    auto actor = actor_zeta::spawn<BenchActor>(&mono_resource);

    for (auto _ : state) {
        state.PauseTiming();
        mono_resource.release();  // Reset buffer for next iteration batch
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

// Unsynchronized pool - fast for single-threaded
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

// Synchronized pool - thread-safe
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

// Default resource (new_delete_resource) - baseline
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

// =============================================================================
// Benchmark: Raw allocation with different allocators
// =============================================================================

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
            // No dealloc for monotonic - just bump
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