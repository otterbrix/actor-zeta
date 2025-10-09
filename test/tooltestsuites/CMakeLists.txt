PROJECT(tooltestsuites LANGUAGES CXX)

file(GLOB ${PROJECT_NAME}_SOURCES
        clock_test.cpp
        scheduler_test.cpp
)

add_library(${PROJECT_NAME}
    ${${PROJECT_NAME}_SOURCES}
)

target_link_libraries(${PROJECT_NAME}
    PUBLIC actor-zeta
)
