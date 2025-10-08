#include <benchmark/benchmark.h>

#include "fixtures.hpp"

/**
 * @brief 
 * 
 * This multithreaded benchmark demonstrates CRITICAL BUG in thread synchronization on workers
 * and blocks indefinitly on condition variable wait.
 * @TODO CRITICAL BUG on condition in cooperative_actor::enqueue_impl: 'flags() != static_cast<int>(state::empty)'
 * infinite waiting on condition_variable in work_sharing::dequeue
 */

REGISTER_BENCHMARKS(size_t, 2);
REGISTER_BENCHMARKS(int8_t, 2);
REGISTER_BENCHMARKS(int16_t, 2);
REGISTER_BENCHMARKS(int32_t, 2);
REGISTER_BENCHMARKS(int64_t, 2);
REGISTER_BENCHMARKS(short, 2);

class memory_manager_t : public benchmark::MemoryManager {
    void Start() BENCHMARK_OVERRIDE {}
    void Stop(Result* result) BENCHMARK_OVERRIDE {
    }
};

// Run the benchmark
int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
        return 1;
    std::unique_ptr<benchmark::MemoryManager> mm(new memory_manager_t());
    benchmark::RegisterMemoryManager(mm.get());
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    benchmark::RegisterMemoryManager(nullptr);
    return 0;
}
