# FetchContent can repeat its patch step when reconfiguring an existing build.
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check --ignore-whitespace "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}" RESULT_VARIABLE _forward OUTPUT_QUIET ERROR_QUIET)
if(_forward EQUAL 0)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${PATCH_FILE}"
        WORKING_DIRECTORY "${SOURCE_DIR}" RESULT_VARIABLE _applied)
    if(NOT _applied EQUAL 0)
        message(FATAL_ERROR "Failed to apply dependency patch: ${PATCH_FILE}")
    endif()
else()
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check --ignore-whitespace "${PATCH_FILE}"
        WORKING_DIRECTORY "${SOURCE_DIR}" RESULT_VARIABLE _reverse OUTPUT_QUIET ERROR_QUIET)
    if(NOT _reverse EQUAL 0)
        message(FATAL_ERROR "Dependency matches neither original nor patched input: ${PATCH_FILE}")
    endif()
    message(STATUS "Dependency patch already applied: ${PATCH_FILE}")
endif()
