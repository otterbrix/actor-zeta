#include <benchmark/benchmark.h>
#include "fixtures.hpp"

// Register benchmarks for different types
REGISTER_BENCHMARKS(int8_t);
REGISTER_BENCHMARKS(int16_t);
REGISTER_BENCHMARKS(int32_t);
REGISTER_BENCHMARKS(int64_t);
REGISTER_BENCHMARKS(size_t);

BENCHMARK_MAIN();