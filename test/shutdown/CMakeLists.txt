set(CURRENT_TEST_NAME shutdown)

find_package(Catch2 2.13 REQUIRED)

set(${CURRENT_TEST_NAME}_SOURCES
        main.cpp
        )

add_executable(${CURRENT_TEST_NAME} ${${CURRENT_TEST_NAME}_SOURCES})

target_link_libraries(
        ${CURRENT_TEST_NAME} PRIVATE
        actor-zeta
        Catch2::Catch2
)

set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )

add_test(NAME ${CURRENT_TEST_NAME} COMMAND ${CURRENT_TEST_NAME})