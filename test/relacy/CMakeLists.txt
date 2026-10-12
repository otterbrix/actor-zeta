# Relacy model of header/actor-zeta/detail/actor_protocol.hpp (ALLOW_RELACY=ON).
#
# Linux only: Relacy runs its threads as ucontext fibers, and on macOS arm64 makecontext() cuts the
# fiber's argument pointer to 32 bits. Locally, run it in Docker:
#
#   docker run --rm -v "$PWD":/src gcc:13 bash -c 'apt-get update -qq && apt-get install -y -qq cmake >/dev/null &&
#     cmake -S /src -B /tmp/b -DALLOW_RELACY=ON && cmake --build /tmp/b --target protocol_model -j &&
#     ctest --test-dir /tmp/b -R protocol_model --output-on-failure'

if (APPLE)
    message(FATAL_ERROR "ALLOW_RELACY: the Relacy model runs on Linux only; see test/relacy/CMakeLists.txt for Docker")
endif ()

include(FetchContent)
# Pinned. SOURCE_SUBDIR names a directory with no CMakeLists.txt, so the sources are only fetched:
# the headers are all the model needs.
FetchContent_Declare(relacy
        GIT_REPOSITORY https://github.com/dvyukov/relacy.git
        GIT_TAG 2c87447b56b90cbccb5129c93a4e82eda5e7868e
        SOURCE_SUBDIR headers-only)
FetchContent_MakeAvailable(relacy)

set(CURRENT_TEST_NAME protocol_model)

add_executable(${CURRENT_TEST_NAME} protocol_model.cpp)

# SYSTEM: Relacy's own warnings are not ours to fix, and CI builds with -Werror.
target_include_directories(${CURRENT_TEST_NAME} SYSTEM PRIVATE ${relacy_SOURCE_DIR})
target_include_directories(${CURRENT_TEST_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/header)

# Relacy names tests through typeid and reports through exceptions: both back on for this target
# only (the last flag wins). The protocol header uses neither.
target_compile_options(${CURRENT_TEST_NAME} PRIVATE -frtti -fexceptions -U__EXCEPTIONS_DISABLE__)

set_target_properties(${CURRENT_TEST_NAME}
        PROPERTIES
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
)

add_test(NAME protocol_model COMMAND ${CURRENT_TEST_NAME})

# Each mutant breaks one rule; exit 0 means the model caught it with the expected result.
foreach (AZ_MUTANT park_ignores_cache leave_before_park sender_ignores_destroying close_forgets_parked)
    add_test(NAME protocol_model_mutant_${AZ_MUTANT} COMMAND ${CURRENT_TEST_NAME} ${AZ_MUTANT})
endforeach ()
