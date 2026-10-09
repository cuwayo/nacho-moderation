# Sanitizer / coverage instrumentation (SPEC-ADDENDUM-TOOLCHAIN A3, A4).
# Selected through the NM_SANITIZE cache variable (set by the CMake presets):
#   none | address | thread | coverage
# ASan and TSan cannot be combined; the presets keep them in separate build trees.
include_guard(GLOBAL)

set(NM_SANITIZE
    "none"
    CACHE STRING "Instrumentation: none, address, thread, or coverage")
set_property(CACHE NM_SANITIZE PROPERTY STRINGS none address thread coverage)

add_library(nm_sanitize INTERFACE)

set(NM_SANITIZE_COMPILE_FLAGS "")
set(NM_SANITIZE_LINK_FLAGS "")

if(NM_SANITIZE STREQUAL "address")
    set(NM_SANITIZE_COMPILE_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all
        -fno-omit-frame-pointer)
    set(NM_SANITIZE_LINK_FLAGS -fsanitize=address,undefined)
elseif(NM_SANITIZE STREQUAL "thread")
    set(NM_SANITIZE_COMPILE_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
    set(NM_SANITIZE_LINK_FLAGS -fsanitize=thread)
elseif(NM_SANITIZE STREQUAL "coverage")
    set(NM_SANITIZE_COMPILE_FLAGS --coverage -fno-omit-frame-pointer)
    set(NM_SANITIZE_LINK_FLAGS --coverage)
elseif(NOT NM_SANITIZE STREQUAL "none")
    message(FATAL_ERROR "Unknown NM_SANITIZE='${NM_SANITIZE}' (want none|address|thread|coverage)")
endif()

if(NM_SANITIZE_COMPILE_FLAGS)
    target_compile_options(nm_sanitize INTERFACE ${NM_SANITIZE_COMPILE_FLAGS})
    target_link_options(nm_sanitize INTERFACE ${NM_SANITIZE_LINK_FLAGS})
endif()
