set(CURRENT_TARGET tests_generator)

find_package(Catch2 REQUIRED)

add_executable(${CURRENT_TARGET}
    main.cpp
)

target_link_libraries(${CURRENT_TARGET}
    PRIVATE
        actor-zeta
        Catch2::Catch2
)

target_include_directories(${CURRENT_TARGET}
    PRIVATE
        ${PROJECT_SOURCE_DIR}/header
)

target_compile_features(${CURRENT_TARGET} PRIVATE cxx_std_20)

# Enable coroutines for GCC (Clang enables coroutines with -std=c++20)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${CURRENT_TARGET} PRIVATE -fcoroutines)
endif()

include(Catch)
catch_discover_tests(${CURRENT_TARGET})