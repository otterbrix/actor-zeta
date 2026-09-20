cmake_minimum_required(VERSION 3.15 FATAL_ERROR)

# Driving a unique_future from outside an actor - requires C++20
if(CMAKE_CXX_STANDARD GREATER_EQUAL 20)
    add_executable(external-drive main.cpp)

    target_link_libraries(
        external-drive
        PRIVATE
        actor-zeta
        Threads::Threads
    )

    set_target_properties(external-drive PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )

    message(STATUS "External-drive example enabled (C++20)")
else()
    message(STATUS "External-drive example disabled (requires C++20, current: C++${CMAKE_CXX_STANDARD})")
endif()
