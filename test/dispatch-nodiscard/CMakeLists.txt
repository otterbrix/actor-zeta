# dispatch() without co_await drops the method's future -- and with it the method's run. Its
# result is [[nodiscard]]: forbidden.cpp, which forgets the co_await, must not build with
# -Werror=unused-result.

try_compile(AZ_DISPATCH_DISCARD_COMPILES
        ${CMAKE_CURRENT_BINARY_DIR}/forbidden
        SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/forbidden.cpp ${PROJECT_SOURCE_DIR}/source/src.cpp
        CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/header;${PROJECT_SOURCE_DIR}"
        COMPILE_DEFINITIONS -Werror=unused-result
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        OUTPUT_VARIABLE AZ_DISPATCH_DISCARD_OUTPUT)

if (AZ_DISPATCH_DISCARD_COMPILES)
    message(FATAL_ERROR "dispatch-nodiscard: a behavior() that drops dispatch()'s future compiled with -Werror=unused-result; "
                        "dispatch() must be [[nodiscard]].")
endif ()
