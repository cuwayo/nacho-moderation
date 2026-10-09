# check_help.cmake - compare `tgbot --help` against the golden file (C8).
#
# Invoked by CTest as:
#   cmake -DNM_TGBOT=<binary> -DNM_GOLDEN=<path> -P tests/cmake/check_help.cmake
# Fails if the binary's help output differs from tests/golden/help.txt, so docs
# and the binary cannot drift.

if(NOT NM_TGBOT OR NOT NM_GOLDEN)
    message(FATAL_ERROR "check_help: NM_TGBOT and NM_GOLDEN must be set")
endif()

execute_process(
    COMMAND "${NM_TGBOT}" --help
    OUTPUT_VARIABLE _actual
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "check_help: '${NM_TGBOT} --help' exited with ${_rc}")
endif()

file(READ "${NM_GOLDEN}" _expected)
if(NOT _actual STREQUAL _expected)
    message(FATAL_ERROR "check_help: --help output differs from ${NM_GOLDEN}")
endif()
