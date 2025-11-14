#include <actor-zeta/config.hpp>

#if HAVE_STD_COROUTINES

#include "fixtures.hpp"

REGISTER_BENCHMARKS(int8_t)
REGISTER_BENCHMARKS(int16_t)
REGISTER_BENCHMARKS(int32_t)
REGISTER_BENCHMARKS(int64_t)

BENCHMARK_MAIN();

#else

#include <benchmark/benchmark.h>
#include <iostream>

static void BM_Dummy(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(42);
    }
}

BENCHMARK(BM_Dummy);

int main(int argc, char** argv) {
    std::cerr << "Warning: Coroutine benchmarks disabled - C++20 coroutines not available\n";
    std::cerr << "Rebuild with C++20 and coroutine support to enable these benchmarks.\n";
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}

#endif