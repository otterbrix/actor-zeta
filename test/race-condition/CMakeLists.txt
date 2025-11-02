find_package(Catch2 2.13 REQUIRED)

# Original race condition stress tests
set(CURRENT_TEST_NAME tests_race_condition)
set(${CURRENT_TEST_NAME}_SOURCES main.cpp)
add_executable(${CURRENT_TEST_NAME} ${${CURRENT_TEST_NAME}_SOURCES})
target_link_libraries(${CURRENT_TEST_NAME} PRIVATE actor-zeta Catch2::Catch2)
set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )
add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})

# Phase 1 Critical Tests: Refcount Management
set(CURRENT_TEST_NAME tests_race_refcount)
set(${CURRENT_TEST_NAME}_SOURCES test_refcount.cpp)
add_executable(${CURRENT_TEST_NAME} ${${CURRENT_TEST_NAME}_SOURCES})
target_link_libraries(${CURRENT_TEST_NAME} PRIVATE actor-zeta Catch2::Catch2)
set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )
add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})

# Phase 1 Critical Tests: Actor Shutdown
set(CURRENT_TEST_NAME tests_race_shutdown)
set(${CURRENT_TEST_NAME}_SOURCES test_shutdown.cpp)
add_executable(${CURRENT_TEST_NAME} ${${CURRENT_TEST_NAME}_SOURCES})
target_link_libraries(${CURRENT_TEST_NAME} PRIVATE actor-zeta Catch2::Catch2)
set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )
add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})

# Phase 1 Critical Tests: State Transitions
set(CURRENT_TEST_NAME tests_race_state_transitions)
set(${CURRENT_TEST_NAME}_SOURCES test_state_transitions.cpp)
add_executable(${CURRENT_TEST_NAME} ${${CURRENT_TEST_NAME}_SOURCES})
target_link_libraries(${CURRENT_TEST_NAME} PRIVATE actor-zeta Catch2::Catch2)
set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )
add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})