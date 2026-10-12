# behavior()'s frame comes from its actor's memory resource, so behavior() must be a member of a
# class with resource(). Anything else must fail to compile -- with the library's own message, not
# an error from deep inside the promise. Checked at configure time.

function(az_require_member_only source_file)
    try_compile(AZ_BEHAVIOR_COMPILES
            ${CMAKE_CURRENT_BINARY_DIR}/try_${source_file}
            SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/${source_file}
            CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/header"
            CXX_STANDARD ${CMAKE_CXX_STANDARD}
            CXX_STANDARD_REQUIRED YES
            OUTPUT_VARIABLE AZ_BEHAVIOR_OUTPUT)
    if (AZ_BEHAVIOR_COMPILES)
        message(FATAL_ERROR "behavior-member-only: ${source_file} compiled; behavior() outside an actor must not.")
    endif ()
    string(FIND "${AZ_BEHAVIOR_OUTPUT}" "must be an actor member function" AZ_MESSAGE_AT)
    if (AZ_MESSAGE_AT EQUAL -1)
        message(FATAL_ERROR "behavior-member-only: ${source_file} failed without the library's message:\n${AZ_BEHAVIOR_OUTPUT}")
    endif ()
endfunction()

az_require_member_only(free_with_message.cpp)
az_require_member_only(free_no_args.cpp)
az_require_member_only(member_without_resource.cpp)
