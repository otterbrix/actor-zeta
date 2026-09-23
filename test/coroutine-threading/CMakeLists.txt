project(tests_coroutine_threading)

find_package(Catch2 REQUIRED)

add_executable(
        tests_coroutine_threading
        main.cpp
)

target_link_libraries(
        tests_coroutine_threading
        PRIVATE
        tooltestsuites
        Catch2::Catch2
)

add_test(
        NAME tests_coroutine_threading
        COMMAND tests_coroutine_threading
)

add_executable(
        tests_coroutine_simple
        simple_test.cpp
)

target_link_libraries(
        tests_coroutine_simple
        PRIVATE
        tooltestsuites
        Catch2::Catch2
)

add_test(
        NAME tests_coroutine_simple
        COMMAND tests_coroutine_simple
)

# Regression guard for the lost wakeup in cooperative_actor::resume_impl: a
# behavior suspended on a pending co_await must not be parked, and an already
# parked one must still have its ready continuation drained.
add_executable(
        tests_coroutine_lost_wakeup
        lost_wakeup_test.cpp
)

target_link_libraries(
        tests_coroutine_lost_wakeup
        PRIVATE
        tooltestsuites
        Catch2::Catch2
)

# Deterministic cases: no threads, no sleeps, no wall clock.
add_test(
        NAME tests_coroutine_lost_wakeup
        COMMAND tests_coroutine_lost_wakeup ~[stress]
)

# Multi-threaded soak, the only coverage of this route under contention. Pass/fail is
# by progress, not elapsed time, so it runs by default; `ctest -LE stress` skips it.
add_test(
        NAME tests_coroutine_lost_wakeup_stress
        COMMAND tests_coroutine_lost_wakeup [stress]
)

set_tests_properties(
        tests_coroutine_lost_wakeup_stress
        PROPERTIES LABELS stress
)