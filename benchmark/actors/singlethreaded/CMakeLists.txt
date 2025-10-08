cmake_minimum_required(VERSION 3.0)

set(project benchmark_actors_singlethreaded)
if (CMAKE_VERSION VERSION_LESS 3.0)
    PROJECT(${project} CXX)
else ()
    cmake_policy(SET CMP0048 NEW)
    PROJECT(${project} VERSION "${CMAKE_PROJECT_VERSION}" LANGUAGES CXX)
endif ()
set(${PROJECT_NAME}_SOURCES
    main.cpp
)
set(${PROJECT_NAME}_HEADERS
    define_actor.hpp
    define_supervisor.hpp
    fixtures.hpp
)
message(STATUS "PROJECT_NAME = ${PROJECT_NAME}")
add_executable(${PROJECT_NAME}
    ${${PROJECT_NAME}_SOURCES}
    ${${PROJECT_NAME}_HEADERS})
target_link_libraries(${PROJECT_NAME} actor-zeta CONAN_PKG::benchmark)
