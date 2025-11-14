project(tests_coroutine_threading)

find_package(Catch2 REQUIRED)

add_executable(
        tests_coroutine_threading
        main.cpp
)

target_link_libraries(
        tests_coroutine_threading
        PRIVATE
        actor-zeta
        tooltestsuites
        Catch2::Catch2
)

add_test(
        NAME tests_coroutine_threading
        COMMAND tests_coroutine_threading
)