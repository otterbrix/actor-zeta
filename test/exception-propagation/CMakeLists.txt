set(CURRENT_TEST_NAME tests_exception_propagation)

# Built WITH exceptions on purpose: the machinery under test only exists in that
# mode. The library source is compiled into this target rather than linked, so the
# whole executable is one compilation mode -- no mixing with the -fno-exceptions
# libactor-zeta.a that the rest of the suite links.
add_executable(${CURRENT_TEST_NAME}
        main.cpp
        ${PROJECT_SOURCE_DIR}/source/src.cpp
)

# Comes after the global -fno-exceptions, and the last flag wins. The project macro
# has to be dropped alongside it: shared_state.hpp #errors when EXCEPTIONS_DISABLE=ON
# contradicts __cpp_exceptions, and here the contradiction is deliberate.
# src.cpp pulls in launch_thread, and the cross-thread case runs a real scheduler.
find_package(Threads REQUIRED)
target_link_libraries(${CURRENT_TEST_NAME} PRIVATE Threads::Threads)

target_compile_options(${CURRENT_TEST_NAME} PRIVATE -fexceptions -U__EXCEPTIONS_DISABLE__)

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

add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})
