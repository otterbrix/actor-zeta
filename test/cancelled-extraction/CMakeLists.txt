set(CURRENT_TEST_NAME tests_cancelled_extraction)

add_executable(${CURRENT_TEST_NAME}
        main.cpp
        ${PROJECT_SOURCE_DIR}/source/src.cpp
)

# NDEBUG on purpose, whatever CMAKE_BUILD_TYPE says: with asserts live the process
# stops at the assert and the defect under test is invisible. NDEBUG is what ships.
target_compile_definitions(${CURRENT_TEST_NAME} PRIVATE NDEBUG)

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

# Control: a future that does hold a value extracts cleanly.
add_test(NAME cancelled_extraction_control COMMAND ${CURRENT_TEST_NAME})

# The defect: extracting from a future with an error and no value must NOT complete
# normally. Plain pass/fail -- the harness traps the refusal itself (see main.cpp),
# so exit 0 means "the library refused" and non-zero means "it handed back a value
# that does not exist". No WILL_FAIL, no output matching: ctest treats a signal as a
# crash regardless of either, and the test should not depend on how the refusal is
# spelled beyond "it does not return".
add_test(NAME cancelled_extraction_refuses COMMAND ${CURRENT_TEST_NAME} cancelled)
