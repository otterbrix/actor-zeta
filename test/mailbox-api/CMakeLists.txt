# A mailbox hands its turn out through push_back() alone: nothing may unpark it without a message.
# Checked at configure time; src.cpp links it, so only the missing member can fail it.
try_compile(AZ_UNBLOCK_COMPILES
        ${CMAKE_CURRENT_BINARY_DIR}/no_unblock
        SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/no_unblock.cpp ${PROJECT_SOURCE_DIR}/source/src.cpp
        CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/header;${PROJECT_SOURCE_DIR}"
        CXX_STANDARD ${CMAKE_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        OUTPUT_VARIABLE AZ_UNBLOCK_OUTPUT)
if (AZ_UNBLOCK_COMPILES)
    message(FATAL_ERROR "mailbox-api: no_unblock.cpp compiled; the mailbox must not offer try_unblock() -- "
                        "it would hand out a second turn.")
endif ()
string(FIND "${AZ_UNBLOCK_OUTPUT}" "try_unblock" AZ_UNBLOCK_AT)
if (AZ_UNBLOCK_AT EQUAL -1)
    message(FATAL_ERROR "mailbox-api: no_unblock.cpp failed for another reason:\n${AZ_UNBLOCK_OUTPUT}")
endif ()
