find_package(Catch2 2.13 REQUIRED)

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
# Sanitizers add 10-50x; every timeout in this file is sized for that.
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 300)

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
# Test 2.1 spawns 1000 threads.
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 600)

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
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 300)

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
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 600)

set(CURRENT_TEST_NAME tests_shutdown_aggressive)
set(${CURRENT_TEST_NAME}_SOURCES test_shutdown_aggressive.cpp)
add_executable(${CURRENT_TEST_NAME} ${${CURRENT_TEST_NAME}_SOURCES})
target_link_libraries(${CURRENT_TEST_NAME} PRIVATE actor-zeta Catch2::Catch2)
set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )
add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 300)

set(CURRENT_TEST_NAME tests_aba_problem)
set(${CURRENT_TEST_NAME}_SOURCES test_aba_problem.cpp)
add_executable(${CURRENT_TEST_NAME} ${${CURRENT_TEST_NAME}_SOURCES})
target_link_libraries(${CURRENT_TEST_NAME} PRIVATE actor-zeta Catch2::Catch2)
set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )
add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 300)

set(CURRENT_TEST_NAME tests_stress_intrusive_ptr)
set(${CURRENT_TEST_NAME}_SOURCES test_stress_intrusive_ptr.cpp)
add_executable(${CURRENT_TEST_NAME} ${${CURRENT_TEST_NAME}_SOURCES})
target_link_libraries(${CURRENT_TEST_NAME} PRIVATE actor-zeta Catch2::Catch2)
set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )
add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 300)

set(CURRENT_TEST_NAME tests_needs_scheduling_race)
set(${CURRENT_TEST_NAME}_SOURCES test_needs_scheduling_race.cpp)
add_executable(${CURRENT_TEST_NAME} ${${CURRENT_TEST_NAME}_SOURCES})
target_link_libraries(${CURRENT_TEST_NAME} PRIVATE actor-zeta Catch2::Catch2)
set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )
add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 300)

set(CURRENT_TEST_NAME tests_available_race)
set(${CURRENT_TEST_NAME}_SOURCES test_available_race.cpp)
add_executable(${CURRENT_TEST_NAME} ${${CURRENT_TEST_NAME}_SOURCES})
target_link_libraries(${CURRENT_TEST_NAME} PRIVATE actor-zeta Catch2::Catch2)
set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )
add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 600)

set(CURRENT_TEST_NAME tests_available_race_chaining)
set(${CURRENT_TEST_NAME}_SOURCES test_available_race_chaining.cpp)
add_executable(${CURRENT_TEST_NAME} ${${CURRENT_TEST_NAME}_SOURCES})
target_link_libraries(${CURRENT_TEST_NAME} PRIVATE actor-zeta Catch2::Catch2)
set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )
add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 600)

set(CURRENT_TEST_NAME tests_shared_state)
set(${CURRENT_TEST_NAME}_SOURCES test_shared_state.cpp)
add_executable(${CURRENT_TEST_NAME} ${${CURRENT_TEST_NAME}_SOURCES})
target_link_libraries(${CURRENT_TEST_NAME} PRIVATE actor-zeta Catch2::Catch2)
set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )
add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 300)

set(CURRENT_TEST_NAME tests_promise_class)
set(${CURRENT_TEST_NAME}_SOURCES test_promise_class.cpp)
add_executable(${CURRENT_TEST_NAME} ${${CURRENT_TEST_NAME}_SOURCES})
target_link_libraries(${CURRENT_TEST_NAME} PRIVATE actor-zeta Catch2::Catch2)
set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )
add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 300)

set(CURRENT_TEST_NAME tests_cas_awaiter)
set(${CURRENT_TEST_NAME}_SOURCES test_cas_awaiter.cpp)
add_executable(${CURRENT_TEST_NAME} ${${CURRENT_TEST_NAME}_SOURCES})
target_link_libraries(${CURRENT_TEST_NAME} PRIVATE actor-zeta Catch2::Catch2)
set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )
add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 300)

set(CURRENT_TEST_NAME tests_self_destroy_coroutine)
set(${CURRENT_TEST_NAME}_SOURCES test_self_destroy_coroutine.cpp)
add_executable(${CURRENT_TEST_NAME} ${${CURRENT_TEST_NAME}_SOURCES})
target_link_libraries(${CURRENT_TEST_NAME} PRIVATE actor-zeta Catch2::Catch2)
set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )
add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 300)

set(CURRENT_TEST_NAME tests_cross_thread)
set(${CURRENT_TEST_NAME}_SOURCES test_cross_thread.cpp)
add_executable(${CURRENT_TEST_NAME} ${${CURRENT_TEST_NAME}_SOURCES})
target_link_libraries(${CURRENT_TEST_NAME} PRIVATE actor-zeta Catch2::Catch2)
set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )
add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 600)
