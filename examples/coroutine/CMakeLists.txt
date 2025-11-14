cmake_minimum_required(VERSION 3.15 FATAL_ERROR)

# Coroutine example - requires C++20
if(CMAKE_CXX_STANDARD GREATER_EQUAL 20)
    add_executable(coroutine mixed_example.cpp)

    target_link_libraries(
        coroutine
        PRIVATE
        actor-zeta
        Threads::Threads
    )

    set_target_properties(coroutine PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )

    message(STATUS "Coroutine example enabled (C++20)")
else()
    message(STATUS "Coroutine example disabled (requires C++20, current: C++${CMAKE_CXX_STANDARD})")
endif()