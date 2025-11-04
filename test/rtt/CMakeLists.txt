PROJECT(tests_rtt CXX)

file(GLOB ${PROJECT_NAME}_HEADERS
    ./*.hpp
    ./*.h
)

file(GLOB ${PROJECT_NAME}_SOURCES
    ./*.cpp
)

find_package(Catch2 2.13 REQUIRED)

message(STATUS "PROJECT_NAME = ${PROJECT_NAME}")


add_executable(${PROJECT_NAME}
    ${${PROJECT_NAME}_SOURCES}
    ${${PROJECT_NAME}_HEADERS})

target_link_libraries(${PROJECT_NAME}
    PUBLIC actor-zeta
    PUBLIC Catch2::Catch2
)

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}../utils")

include(CTest)
include(Catch)

# Increase timeout for all tests in this executable (especially rt_tuple with sanitizers)
# ASan makes test ~10x slower due to debug output
# TSan makes test ~100x slower due to instrumentation
catch_discover_tests(${PROJECT_NAME}
    PROPERTIES TIMEOUT 300
)
