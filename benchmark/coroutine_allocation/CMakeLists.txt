set(PROJECT_NAME benchmark_coroutine_allocation)
project(${PROJECT_NAME})

find_package(benchmark REQUIRED)

add_executable(${PROJECT_NAME}
    main.cpp
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        actor-zeta
        benchmark::benchmark
        benchmark::benchmark_main
)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_SOURCE_DIR}/header
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)