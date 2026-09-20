# A behavior may co_await exactly two things: a unique_future<T>, and the
# pair<bool, unique_future<T>> that send() returns -- and the second of those is
# itself a static_assert now. Nothing else, and no co_yield.
#
# The prohibition is not cosmetic. await_transform is a MEMBER of the promise, so
# [expr.await]/3.2 routes every co_await in a behavior through it, which means every
# suspension point runs propagate_awaited_state() and publishes awaited_flags_. That
# is what makes "a suspended behavior is always is_busy()" true, and in turn what
# keeps park() -- which pairs `awaiting` with no job in any queue -- unreachable for
# a live behavior. A third overload, or a yield_value, would be a suspension point
# that skips the publication and would take the invariant with it, silently.
#
# Checked at configure time, because the property under test is the ABSENCE of an
# overload and no amount of running the permitted shapes would notice a new one.

set(CURRENT_TEST_NAME tests_foreign_awaitable_prohibited)

function(az_require_no_compile source_file message)
    try_compile(AZ_IT_COMPILED
            "${CMAKE_CURRENT_BINARY_DIR}/try_${source_file}"
            SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}"
            CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/header"
            CXX_STANDARD ${CMAKE_CXX_STANDARD}
            CXX_STANDARD_REQUIRED YES)
    if (AZ_IT_COMPILED)
        message(FATAL_ERROR "${source_file} compiled. ${message}")
    endif ()
endfunction()

az_require_no_compile(forbidden_awaitable.cpp
        "A behavior must not co_await a foreign awaitable: that is a suspension point \
that never publishes awaited_flags_, so the behavior can be suspended and not \
is_busy(), and park() becomes reachable for a live behavior.")

az_require_no_compile(forbidden_yield.cpp
        "behavior_t::promise_type must not grow a yield_value: co_yield would be a \
suspension point outside await_transform, with the same consequence.")

add_executable(${CURRENT_TEST_NAME} allowed.cpp)

target_link_libraries(${CURRENT_TEST_NAME} PRIVATE actor-zeta)

set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
)

add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})
