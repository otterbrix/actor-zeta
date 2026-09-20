set(CURRENT_TEST_NAME tests_shutdown_sender_race)

add_executable(${CURRENT_TEST_NAME} main.cpp)

target_link_libraries(${CURRENT_TEST_NAME} PRIVATE actor-zeta)

set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
)

# Times out rather than hangs the suite if the destructor never lets go.
add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 30)
