PROJECT(benchmark_message CXX)

set(${PROJECT_NAME}_SOURCES
    message.cpp
)

set(${PROJECT_NAME}_HEADERS
    fixtures.hpp
    register_benchmark.hpp
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

###########################################################################################
PROJECT(benchmark_body_rtt CXX)

set(${PROJECT_NAME}_SOURCES
    body_rtt.cpp
)

set(${PROJECT_NAME}_HEADERS
    fixtures.hpp
    register_benchmark.hpp
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
