cmake_minimum_required(VERSION 3.15 FATAL_ERROR)

# Standalone-Asio example - requires C++20 and the optional asio dependency.
# Only added when ALLOW_ASIO_EXAMPLE=ON (see examples/CMakeLists.txt), which also
# defines ACTOR_ZETA_WITH_ASIO. Guard on C++20 like the other coroutine examples.
if(CMAKE_CXX_STANDARD GREATER_EQUAL 20)
    # Conan (CMakeDeps) provides the standalone, header-only asio package as the
    # imported target asio::asio.
    find_package(asio REQUIRED)

    add_executable(asio_example main.cpp)

    target_link_libraries(
        asio_example
        PRIVATE
        actor-zeta
        asio::asio
        Threads::Threads
    )

    # Standalone Asio (no Boost) + no-exceptions/no-rtti consistent with the project.
    # ASIO_NO_EXCEPTIONS routes every Asio throw site through the user-supplied
    # asio::detail::throw_exception(...) defined in asio_no_exceptions.hpp.
    # ASIO_NO_TYPEID makes Asio use its own type-id fallback instead of typeid(),
    # required because the project builds with -fno-rtti (asio::awaitable / io_context
    # use typeid in their executor machinery otherwise).
    target_compile_definitions(asio_example PRIVATE
        ASIO_STANDALONE
        ASIO_NO_EXCEPTIONS
        ASIO_NO_TYPEID
    )

    # Mirror the project-wide -fno-exceptions for this target explicitly so the
    # example stays exception-free even if built outside the top-level flags.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "AppleClang")
        target_compile_options(asio_example PRIVATE -fno-exceptions)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
        target_compile_options(asio_example PRIVATE /EHsc)
    endif()

    # Enable coroutines for GCC (Clang enables coroutines with -std=c++20).
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(asio_example PRIVATE -fcoroutines)
    endif()

    set_target_properties(asio_example PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    message(STATUS "Asio example enabled (C++20, standalone Asio, no exceptions)")
else()
    message(STATUS "Asio example disabled (requires C++20, current: C++${CMAKE_CXX_STANDARD})")
endif()
