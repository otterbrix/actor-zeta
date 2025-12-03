project(tests_dispatcher_pattern)

find_package(Catch2 REQUIRED)

add_executable(
        tests_dispatcher_pattern
        main.cpp
)

target_link_libraries(
        tests_dispatcher_pattern
        PRIVATE
        actor-zeta
        tooltestsuites
        Catch2::Catch2
)

add_test(
        NAME tests_dispatcher_pattern
        COMMAND tests_dispatcher_pattern
)