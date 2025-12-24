cmake_minimum_required(VERSION 3.15 FATAL_ERROR)

# Generator example - requires C++20
if(CMAKE_CXX_STANDARD GREATER_EQUAL 20)
    add_executable(generator main.cpp)

    target_link_libraries(
        generator
        PRIVATE
        actor-zeta
        Threads::Threads
    )

    set_target_properties(generator PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )

    # Enable coroutines for GCC (Clang enables coroutines with -std=c++20)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(generator PRIVATE -fcoroutines)
    endif()

    message(STATUS "Generator example enabled (C++20)")
else()
    message(STATUS "Generator example disabled (requires C++20, current: C++${CMAKE_CXX_STANDARD})")
endif()