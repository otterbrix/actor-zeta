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
# TSan adds 10-50x overhead for race condition stress tests
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 300)

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
# TSan adds 10-50x overhead; 1000 thread spawns in Test 2.1 needs extra time
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 600)

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
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 300)

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
# TSan adds 10-50x overhead; state transition stress tests need extra time
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 600)

# Aggressive Shutdown Tests: Race condition reproduction
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

# ABA Problem Stress Tests: CAS operations and lock-free queue
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

# Intrusive Ptr Stress Tests: Refcount operations and pointer operations
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

# needs_scheduling() Race Condition Tests
# Tests for race between ~resume_guard() and try_schedule_after_enqueue()
# See docs/actor-zeta-race-condition.md
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

# available() Race Condition Tests
# Tests for race between available() and final_suspend()
# See docs/actor-zeta-available-race.md
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
# ASan/TSan makes this test slower; increase timeout
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 600)

# available() Race with Coroutine Chaining - exact reproduction of otterbrix bug
# Tests for race between available() and final_suspend() with co_await chains
# See docs/actor-zeta-race-fix-proposal.md
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
# Chaining tests need more time with sanitizers
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 600)

# =============================================================================
# NEW ARCHITECTURE TESTS (docs/actor-zeta-race-comprehensive-fix.md)
# These tests verify the new self-destroying coroutine pattern with
# Last-One-Out ownership model and CAS-based awaiter.
# =============================================================================

# shared_state<T> Tests: atomic flags, Last-One-Out ownership
# See docs/actor-zeta-race-comprehensive-fix.md §5.1
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
# Concurrent tests need extra time with TSan
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 300)

# promise<T> Class Tests: Seastar-style API, ownership transfer
# See docs/actor-zeta-race-comprehensive-fix.md §12.3
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

# CAS-based Awaiter Tests: Variant B+ with CAS for continuation
# See docs/actor-zeta-race-comprehensive-fix.md §7.2
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
# CAS stress tests need extra time
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 300)

# Self-Destroying Coroutine Tests: Core of the race condition fix
# See docs/actor-zeta-race-comprehensive-fix.md §6.2
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
# Self-destroy tests need extra time with ASan
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 300)

# Cross-Thread Stress Tests: unique_future in different thread than producer
# See docs/cross-thread-future-fix.md
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
# Cross-thread tests need extra time with sanitizers
set_tests_properties(${CURRENT_TEST_NAME} PROPERTIES TIMEOUT 600)
