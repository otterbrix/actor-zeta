##### cached_queue ################################################################################
PROJECT(tests_cached_queue CXX)

file(GLOB ${PROJECT_NAME}_HEADERS
    ./*.hpp
    ./*.h
)

file(GLOB ${PROJECT_NAME}_SOURCES
    ./cached_queue.cpp
)

find_package(Catch2 2.13 REQUIRED)

message(STATUS "PROJECT_NAME = ${PROJECT_NAME}")

add_executable(${PROJECT_NAME}
    ${${PROJECT_NAME}_SOURCES}
    ${${PROJECT_NAME}_HEADERS})

target_link_libraries(${PROJECT_NAME}
    PUBLIC actor-zeta
    PUBLIC Catch2::Catch2
)

include(CTest)
include(Catch)
catch_discover_tests(${PROJECT_NAME})

##### fifo_inbox ##################################################################################
PROJECT(tests_fifo_inbox CXX)

file(GLOB ${PROJECT_NAME}_HEADERS
    ./*.hpp
    ./*.h
)

file(GLOB ${PROJECT_NAME}_SOURCES
    ./fifo_inbox.cpp
)

message(STATUS "PROJECT_NAME = ${PROJECT_NAME}")

add_executable(${PROJECT_NAME}
    ${${PROJECT_NAME}_SOURCES}
    ${${PROJECT_NAME}_HEADERS})

target_link_libraries(${PROJECT_NAME}
    PUBLIC actor-zeta
    PUBLIC Catch2::Catch2
)

include(CTest)
include(Catch)
catch_discover_tests(${PROJECT_NAME})

##### fixed_queue #################################################################################
PROJECT(tests_fixed_queue CXX)

file(GLOB ${PROJECT_NAME}_HEADERS
    ./*.hpp
    ./*.h
)

file(GLOB ${PROJECT_NAME}_SOURCES
    ./fixed_queue.cpp
)

message(STATUS "PROJECT_NAME = ${PROJECT_NAME}")

add_executable(${PROJECT_NAME}
    ${${PROJECT_NAME}_SOURCES}
    ${${PROJECT_NAME}_HEADERS})

target_link_libraries(${PROJECT_NAME}
    PUBLIC actor-zeta
    PUBLIC Catch2::Catch2
)

include(CTest)
include(Catch)
catch_discover_tests(${PROJECT_NAME})

##### lifo_inbox ##################################################################################
PROJECT(tests_lifo_inbox CXX)

file(GLOB ${PROJECT_NAME}_HEADERS
    ./*.hpp
    ./*.h
)

file(GLOB ${PROJECT_NAME}_SOURCES
    ./lifo_inbox.cpp
)

message(STATUS "PROJECT_NAME = ${PROJECT_NAME}")

add_executable(${PROJECT_NAME}
    ${${PROJECT_NAME}_SOURCES}
    ${${PROJECT_NAME}_HEADERS})

target_link_libraries(${PROJECT_NAME}
    PUBLIC actor-zeta
    PUBLIC Catch2::Catch2
)

include(CTest)
include(Catch)
catch_discover_tests(${PROJECT_NAME})

##### queue #######################################################################################
PROJECT(tests_queue CXX)

file(GLOB ${PROJECT_NAME}_HEADERS
    ./*.hpp
    ./*.h
)

file(GLOB ${PROJECT_NAME}_SOURCES
    ./queue.cpp
)

message(STATUS "PROJECT_NAME = ${PROJECT_NAME}")

add_executable(${PROJECT_NAME}
    ${${PROJECT_NAME}_SOURCES}
    ${${PROJECT_NAME}_HEADERS})

target_link_libraries(${PROJECT_NAME}
    PUBLIC actor-zeta
    PUBLIC Catch2::Catch2
)

include(CTest)
include(Catch)
catch_discover_tests(${PROJECT_NAME})

##### task_queue ##################################################################################
PROJECT(tests_task_queue CXX)

file(GLOB ${PROJECT_NAME}_HEADERS
    ./*.hpp
    ./*.h
)

file(GLOB ${PROJECT_NAME}_SOURCES
    ./task_queue.cpp
)

message(STATUS "PROJECT_NAME = ${PROJECT_NAME}")

add_executable(${PROJECT_NAME}
    ${${PROJECT_NAME}_SOURCES}
    ${${PROJECT_NAME}_HEADERS})

target_link_libraries(${PROJECT_NAME}
    PUBLIC actor-zeta
    PUBLIC Catch2::Catch2
)

include(CTest)
include(Catch)
catch_discover_tests(${PROJECT_NAME})
