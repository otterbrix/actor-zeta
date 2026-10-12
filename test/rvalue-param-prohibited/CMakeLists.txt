# A `T&&` method parameter must not compile, for any T: arguments cross the actor
# boundary by value, the same way a result does (unique_future<T> rejects references).
# A `T&&` would refer into the message body instead of owning the argument.
#
# Checked at configure time: try_compile the forbidden shape and fail if it builds. A
# test that only ran the right shape would not notice the wrong one coming back.

set(CURRENT_TEST_NAME tests_rvalue_param_prohibited)

try_compile(AZ_RVALUE_PARAM_COMPILES
        "${CMAKE_CURRENT_BINARY_DIR}/try"
        SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/forbidden.cpp"
        CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/header"
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        OUTPUT_VARIABLE AZ_RVALUE_PARAM_LOG)

if (AZ_RVALUE_PARAM_COMPILES)
    message(FATAL_ERROR
            "A method with a std::string&& parameter compiled. It must not: arguments are "
            "passed by value. Restore the T&& check in actor/dispatch_traits.hpp.")
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
