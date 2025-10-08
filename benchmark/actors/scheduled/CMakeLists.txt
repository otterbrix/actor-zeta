PROJECT(benchmark_actors_ping_pong_scheduled CXX)

set(${PROJECT_NAME}_SOURCES
    counters.cpp
    main.cpp
)

set(${PROJECT_NAME}_HEADERS
    counters.h
    fixtures.hpp
    enums.hpp
    define_actor.hpp
    define_supervisor.hpp
)

message(STATUS "PROJECT_NAME = ${PROJECT_NAME}")

add_executable(${PROJECT_NAME}
    ${${PROJECT_NAME}_SOURCES}
    ${${PROJECT_NAME}_HEADERS}
)

if (${CMAKE_CXX_COMPILER_ID} MATCHES "Clang") #------------------------------------- Clang
    target_link_libraries(${PROJECT_NAME}
        PUBLIC ${REQ_LLVM_LIBRARIES}
    )
endif() #--------------------------------------------------------------------------- Clang

target_link_libraries(${PROJECT_NAME}
    PUBLIC actor-zeta 
    PUBLIC CONAN_PKG::benchmark
)
