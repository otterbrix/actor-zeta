add_subdirectory(balancer)
#add_subdirectory(dataflow)
add_subdirectory(broadcast)
add_subdirectory(supervisor)
add_subdirectory(coroutine)
add_subdirectory(generator)
add_subdirectory(delegation)

# Optional standalone-Asio example.
# OFF by default so the core build pulls in ZERO Asio headers.
# When ON, define ACTOR_ZETA_WITH_ASIO for the asio subtree and add it.
option(ALLOW_ASIO_EXAMPLE "ALLOW ASIO EXAMPLE" OFF)
message(STATUS "ALLOW_ASIO_EXAMPLE = ${ALLOW_ASIO_EXAMPLE}")
if (ALLOW_ASIO_EXAMPLE)
    add_compile_definitions(ACTOR_ZETA_WITH_ASIO)
    add_subdirectory(asio)
endif()
