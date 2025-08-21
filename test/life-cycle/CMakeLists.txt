PROJECT(tests_life_cycle CXX)

find_package(Catch2 2.13.7 REQUIRED)

message(STATUS "PROJECT_NAME = ${PROJECT_NAME}")

add_executable(${PROJECT_NAME} main.cpp)

target_link_libraries(${PROJECT_NAME}
    PUBLIC actor-zeta
    PUBLIC tooltestsuites
    PUBLIC Catch2::Catch2
)

include(CTest)
include(Catch)
catch_discover_tests(${PROJECT_NAME})
