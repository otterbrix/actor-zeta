#include <actor-zeta/config.hpp>
#include "fixtures.hpp"

REGISTER_BENCHMARKS(int8_t)
REGISTER_BENCHMARKS(int16_t)
REGISTER_BENCHMARKS(int32_t)
REGISTER_BENCHMARKS(int64_t)

BENCHMARK_MAIN();