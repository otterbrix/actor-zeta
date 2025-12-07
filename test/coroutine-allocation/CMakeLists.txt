set(PROJECT_NAME tests_coroutine_allocation)
project(${PROJECT_NAME})

find_package(Catch2 REQUIRED)
include(Catch)

add_executable(${PROJECT_NAME}
    main.cpp
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        actor-zeta
        Catch2::Catch2
)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_SOURCE_DIR}/header
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

catch_discover_tests(${PROJECT_NAME})