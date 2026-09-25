set(CURRENT_TEST_NAME tests_protocol_violations)

add_executable(${CURRENT_TEST_NAME}
        main.cpp
        ${PROJECT_SOURCE_DIR}/source/src.cpp
)

# The checks under test include asserts: keep them live whatever CMAKE_BUILD_TYPE says.
# Compile options follow the build type's flags on the command line, so -UNDEBUG wins over -DNDEBUG.
target_compile_options(${CURRENT_TEST_NAME} PRIVATE -UNDEBUG)

target_include_directories(${CURRENT_TEST_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/header
)

set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
)

# One ctest per check; main.cpp traps SIGABRT, so exit 0 means "stopped". No WILL_FAIL,
# since ctest treats a signal as a crash regardless. await_neighbour is a control: it traps
# nothing and must finish.
foreach (AZ_CHECK second_get_future nested_resume resume_without_turn resume_after_close double_delete delete_from_inside
                  self_await await_neighbour)
    add_test(NAME protocol_violation_${AZ_CHECK} COMMAND ${CURRENT_TEST_NAME} ${AZ_CHECK})
endforeach ()
# delete_from_inside waits out the bounded wait for the actor to drain.
set_tests_properties(protocol_violation_delete_from_inside PROPERTIES TIMEOUT 120)

# The same checks as they ship: NDEBUG on, so debug-only checks are gone. Only the checks that stay
# on in release run here.
set(AZ_RELEASE_CHECKS tests_protocol_violations_release)
add_executable(${AZ_RELEASE_CHECKS}
        main.cpp
        ${PROJECT_SOURCE_DIR}/source/src.cpp
)
target_compile_definitions(${AZ_RELEASE_CHECKS} PRIVATE NDEBUG)
target_include_directories(${AZ_RELEASE_CHECKS} PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/header
)
set_target_properties(${AZ_RELEASE_CHECKS}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
)
foreach (AZ_CHECK nested_resume resume_without_turn resume_after_close double_delete)
    add_test(NAME protocol_violation_release_${AZ_CHECK} COMMAND ${AZ_RELEASE_CHECKS} ${AZ_CHECK})
endforeach ()
