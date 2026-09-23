add_subdirectory(balancer)
add_subdirectory(broadcast)
add_subdirectory(supervisor)
add_subdirectory(coroutine)
add_subdirectory(delegation)
add_subdirectory(external-drive)

# OFF by default so the core build pulls in zero Asio headers.
option(ALLOW_ASIO_EXAMPLE "ALLOW ASIO EXAMPLE" OFF)
message(STATUS "ALLOW_ASIO_EXAMPLE = ${ALLOW_ASIO_EXAMPLE}")
if (ALLOW_ASIO_EXAMPLE)
    add_compile_definitions(ACTOR_ZETA_WITH_ASIO)
    add_subdirectory(asio)
endif()
