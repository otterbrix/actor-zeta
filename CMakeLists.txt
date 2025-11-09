cmake_minimum_required(VERSION 3.15)
PROJECT(actor-zeta LANGUAGES C CXX)

message(STATUS "CMAKE_CXX_COMPILER_ID = ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "CMAKE_C_COMPILER_ID = ${CMAKE_C_COMPILER_ID}")
message(STATUS "STANDARD CPP = ${CMAKE_CXX_STANDARD}")
message(STATUS "CMAKE_BUILD_TYPE = ${CMAKE_BUILD_TYPE}")

if (${CMAKE_CXX_COMPILER_ID} MATCHES "Clang"  AND UNIX AND NOT APPLE ) #------------------------------------- Clang
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND CMAKE_MODULE_PATH "${LLVM_DIR}")
endif() #--------------------------------------------------------------------------- Clang

set(CMAKE_CXX_STANDARD 17 CACHE STRING "The C++ standard whose features are requested.")
set(CMAKE_CXX_STANDARD_REQUIRED     ON)
set(CMAKE_CXX_EXTENSIONS            OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS   ON)

option(ALLOW_EXAMPLES               "ALLOW EXAMPLES"            OFF)
option(ALLOW_TESTS                  "ALLOW TESTS"               OFF)
option(ENABLE_TESTS_MEASUREMENTS    "ENABLE TESTS MEASUREMENTS" OFF)
option(ALLOW_BENCHMARK              "ALLOW BENCHMARK"           OFF)
option(RTTI_DISABLE                 "RTTI DISABLE"              ON)
option(EXCEPTIONS_DISABLE           "EXCEPTIONS DISABLE"        ON)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (default Debug)" FORCE)
endif()

# RTTI/Exceptions flags
if (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    if (EXCEPTIONS_DISABLE)
        message(STATUS "Disable C++ exceptions (MSVC).")
        string(REGEX REPLACE "/EH[a-z]+" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc")
        add_compile_definitions(__EXCEPTIONS_DISABLE__)
    endif()

    if (RTTI_DISABLE)
        message(STATUS "Disable RTTI (MSVC).")
        string(REGEX REPLACE "/GR" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GR-")
    endif()
elseif ((CMAKE_CXX_COMPILER_ID MATCHES "GNU") OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang") OR (CMAKE_CXX_COMPILER_ID MATCHES "AppleClang"))
    if (EXCEPTIONS_DISABLE)
        message(STATUS "Disable C++ exceptions (GNU/Clang).")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions")
        add_compile_definitions(__EXCEPTIONS_DISABLE__)
    endif()

    if (RTTI_DISABLE)
        message(STATUS "Disable RTTI (GNU/Clang).")
        string(REGEX REPLACE "-frtti" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
    endif()
endif()

message(STATUS "ALLOW_EXAMPLES = ${ALLOW_EXAMPLES}")
message(STATUS "ALLOW_TESTS = ${ALLOW_TESTS}")
message(STATUS "ENABLE_TESTS_MEASUREMENTS = ${ENABLE_TESTS_MEASUREMENTS}")
message(STATUS "ALLOW_BENCHMARK = ${ALLOW_BENCHMARK}")
message(STATUS "RTTI_DISABLE = ${RTTI_DISABLE}")
message(STATUS "EXCEPTIONS_DISABLE = ${EXCEPTIONS_DISABLE}")

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/
    ${CMAKE_CURRENT_SOURCE_DIR}/header)

if ((CMAKE_CXX_COMPILER_ID MATCHES "GNU") OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    add_compile_options(
            -Wall
            -Wextra
            -Wpedantic

            -Wcast-align
            -Wcast-qual
            -Wconversion
            -Wctor-dtor-privacy
            -Wenum-compare
            -Wfloat-equal
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Woverloaded-virtual
            -Wredundant-decls
            -Wsign-conversion
            -Wsign-promo
    )
elseif (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    add_compile_options(/W4 /WX)
endif()

############## FINAL PROJECT CONFIG #################

file(GLOB ${PROJECT_NAME}_HEADERS
    header/actor-zeta/base/*.hpp
    header/actor-zeta/base/*.h

    header/actor-zeta/detail/queue/*.hpp
    header/actor-zeta/detail/queue/*.h

    header/actor-zeta/detail/pmr/*.hpp
    header/actor-zeta/detail/pmr/*.h

    header/actor-zeta/detail/*.hpp
    header/actor-zeta/detail/*.h

    header/actor-zeta/scheduler/policy/*.hpp
    header/actor-zeta/scheduler/policy/*.h

    header/actor-zeta/scheduler/*.hpp
    header/actor-zeta/scheduler/*.h

    header/actor-zeta/mailbox/*.hpp
    header/actor-zeta/mailbox/*.h

    header/actor-zeta/*.hpp
    header/actor-zeta/*.h

    header/*.hpp
    header/*.h
)

if ((CMAKE_CXX_COMPILER_ID MATCHES "Clang") AND NOT APPLE) #------------------------------------- Clang
    ############## LLVM CONFIGURATION #################
    # https://github.com/nsumner/llvm-demo Example LLVM Build

    # LLVM_DIR must be set to the prefix of /share/llvm/cmake via commandline
    find_package(LLVM REQUIRED CONFIG)
    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_LIBRARY_DIRS}")

    # Check if using Conan toolchain (to avoid stdlib conflicts with dependencies)
    if (DEFINED CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "conan")
        # Conan build: use libstdc++11 to match Conan-built dependencies
        set(LIBCXX "libstdc++11")
        message(STATUS "Conan build detected: forcing LIBCXX=${LIBCXX} to match dependencies")
    else()
        # Auto-detect stdlib if not using Conan
        find_library(CLANG_LIBCPP
                NAMES libc++ c++
                HINTS ${LLVM_LIBRARY_DIRS})

        find_library(CLANG_LIBCPP_ABI
                NAMES libc++abi c++abi
                HINTS ${LLVM_LIBRARY_DIRS})

        message(STATUS "Found CLANG_LIBCPP=${CLANG_LIBCPP}")
        message(STATUS "Found CLANG_LIBCPP_ABI=${CLANG_LIBCPP_ABI}")

        # Linker flags for targets and Conan
        set(LINKFLAGS "")
        ### https://stackoverflow.com/questions/43662736/cmake-switch-between-clang-g-and-libc-libstdc

        if (${CLANG_LIBCPP} MATCHES "CLANG_LIBCPP-NOTFOUND")
            set(LIBCXX "libstdc++11")
        else()
            # Change compiler library and libunwind from libgcc GCC implementation to LLVM's compiler-rt
            # See https://clang.llvm.org/docs/Toolchain.html for details
            # LINKFLAGS variable contains required link flags
            set(LINKFLAGS "-stdlib=libc++ -rtlib=compiler-rt")

            if (NOT APPLE)
                find_library(
                        LIBUNWIND_LIBRARY
                        NAMES libunwind.a
                        PATH_SUFFIXES lib lib64)

                if (LIBUNWIND_LIBRARY)
                    message(STATUS "libunwind found: ${LIBUNWIND_LIBRARY}, using libunwind")
                    set(LINKFLAGS "${LINKFLAGS} -unwindlib=libunwind")
                else()
                    message(STATUS "libunwind not found, using libgcc")
                    set(LINKFLAGS "${LINKFLAGS} -unwindlib=libgcc -lgcc_s")
                endif()
            endif()
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${LINKFLAGS}")
            set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${LINKFLAGS}")
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
            set(LIBCXX "libc++")
        endif()

        if (CLANG_LIBCPP_ABI MATCHES "CLANG_LIBCPP_ABI-NOTFOUND")
        else()
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lc++abi")
        endif()
    endif()

else() #---------------------------------------------------------------------------- GNU
    set(LIBCXX "libstdc++11")
endif() #--------------------------------------------------------------------------- Clang

message(STATUS "Using shared linker flags : ${CMAKE_SHARED_LINKER_FLAGS}")
message(STATUS "Using exe linker flags    : ${CMAKE_EXE_LINKER_FLAGS}")

add_library(
    ${PROJECT_NAME}
    source/src.cpp
    ${${PROJECT_NAME}_HEADERS}
)

find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PUBLIC
        Threads::Threads
        ${CMAKE_DL_LIBS}
)

#################################################################

# configuration
#############################################################################################
#conan

if (CONAN_EXPERIMENTAL_INCLUDE)
    set(CONAN_SYSTEM_INCLUDES "On")
endif ()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang"  AND UNIX AND NOT APPLE )
    # https://docs.conan.io/1/reference/config_files/settings.yml.html#c-standard-libraries-aka-compiler-libcxx
    #conan_cmake_autodetect(CONAN_SETTINGS)
    set(CONAN_SETTINGS "build_type=${CMAKE_BUILD_TYPE};compiler.libcxx=${LIBCXX};compiler.cppstd=${CMAKE_CXX_STANDARD}")
endif ()

message(STATUS "CONAN_SETTINGS: ${CONAN_SETTINGS}")


# configuration
#############################################################################################
#conan

if (ALLOW_EXAMPLES)
    add_subdirectory(examples)
endif()

if (ALLOW_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(test)
endif()

if (ALLOW_BENCHMARK)
    # add_subdirectory(benchmark)
endif()
