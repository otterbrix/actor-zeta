# `co_await send(...)` must not compile.
#
# The shape suspends on a future whose producer nobody scheduled: send() hands the
# scheduling obligation back as needs_sched, and an awaiter can only surface it from
# await_resume(), i.e. after the wait that cannot end without it. The actor then
# spins in the run queue forever with no diagnostic, so the prohibition has to be a
# compile error -- it is decidable from the type alone.
#
# Checked at configure time: try_compile the forbidden shape and fail loudly if it
# builds. A test that merely ran the right shape would not notice the wrong one
# coming back.

set(CURRENT_TEST_NAME tests_co_await_send_prohibited)

try_compile(AZ_FORBIDDEN_SHAPE_COMPILES
        "${CMAKE_CURRENT_BINARY_DIR}/try"
        SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/forbidden.cpp"
        CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/header"
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        OUTPUT_VARIABLE AZ_FORBIDDEN_SHAPE_LOG)

if (AZ_FORBIDDEN_SHAPE_COMPILES)
    message(FATAL_ERROR
            "co_await send(...) compiled. It must not: the shape suspends forever "
            "because needs_sched is delivered only after the await. Restore the "
            "static_assert in detail/future_awaiters.hpp.")
endif ()

# The right shape, on the other hand, must build and run.
add_executable(${CURRENT_TEST_NAME} allowed.cpp)

target_link_libraries(${CURRENT_TEST_NAME} PRIVATE actor-zeta)

set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
)

add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})
