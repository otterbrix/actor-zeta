PROJECT(benchmark_actors_ping_pong_scheduled CXX)

find_package(benchmark REQUIRED)

message(STATUS "PROJECT_NAME = ${PROJECT_NAME}")

# Ping-pong benchmark with dispatch_traits
add_executable(${PROJECT_NAME}
    new_benchmark.cpp
)

target_link_libraries(${PROJECT_NAME}
    PUBLIC actor-zeta
    PUBLIC benchmark::benchmark
)