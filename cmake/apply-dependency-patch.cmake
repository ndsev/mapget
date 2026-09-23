# FetchContent may rerun PATCH_COMMAND on reconfigure. Accept an already-applied
# patch, but fail on incompatible source instead of silently building without it.
find_package(Git REQUIRED)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE already_applied
    OUTPUT_QUIET ERROR_QUIET)
if (already_applied EQUAL 0)
    return()
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output ERROR_VARIABLE error)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "Failed to apply ${PATCH_FILE}: ${output}${error}")
endif()
