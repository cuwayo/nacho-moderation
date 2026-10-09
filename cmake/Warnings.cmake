# Warning flags (SPEC-ADDENDUM-TOOLCHAIN A3). Warnings are errors.
# Provides an INTERFACE target `nm_warnings` that every first-party target links.
include_guard(GLOBAL)

add_library(nm_warnings INTERFACE)

set(NM_WARNING_FLAGS
    -std=c11
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wformat=2
    -Wformat-security
    -Wnull-dereference
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wcast-align
    -Wundef
    -Wvla
    -Werror)

target_compile_options(nm_warnings INTERFACE ${NM_WARNING_FLAGS})

# GCC additionally supports -fanalyzer; it is slow, so it is opt-in via NM_ANALYZER.
option(NM_ANALYZER "Enable GCC -fanalyzer on first-party targets" OFF)
if(NM_ANALYZER AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(nm_warnings INTERFACE -fanalyzer)
endif()
