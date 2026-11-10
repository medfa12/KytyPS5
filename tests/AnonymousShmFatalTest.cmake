if(NOT DEFINED TEST_EXECUTABLE OR NOT EXISTS "${TEST_EXECUTABLE}")
	message(FATAL_ERROR "Anonymous SHM test executable is required")
endif()
if(MODE STREQUAL "unmap")
	set(expected "Anonymous SHM: unmap restoration failed")
elseif(MODE STREQUAL "protect")
	set(expected "Anonymous SHM: protection restoration failed")
else()
	message(FATAL_ERROR "Unknown anonymous SHM fatal-test mode")
endif()

execute_process(COMMAND "${TEST_EXECUTABLE}" "--anonymous-shm-fatal-${MODE}"
	TIMEOUT 45 RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
# An arbitrary crash, timeout, fixture assertion, or returned syscall is not
# evidence that the production restoration boundary was reached.
if(NOT "${result}" STREQUAL "321")
	message(FATAL_ERROR "Expected fatal exit 321; got ${result}\n${output}\n${error}")
endif()
string(FIND "${output}\n${error}" "${expected}" marker)
if(marker EQUAL -1)
	message(FATAL_ERROR "Missing exact restoration diagnostic: ${expected}\n${output}\n${error}")
endif()
message(STATUS "Observed fatal restoration boundary: ${expected}")
