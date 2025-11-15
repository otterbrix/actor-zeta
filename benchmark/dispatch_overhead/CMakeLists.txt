find_package(benchmark REQUIRED)

add_executable(benchmark_dispatch_overhead main.cpp)

target_link_libraries(
    benchmark_dispatch_overhead
    PRIVATE
        actor-zeta
        benchmark::benchmark
)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(benchmark_dispatch_overhead PRIVATE -O3 -march=native)
endif()