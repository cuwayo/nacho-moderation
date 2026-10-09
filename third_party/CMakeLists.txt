# Vendored dependencies (SPEC-ADDENDUM-TOOLCHAIN A1, A2): cmocka (test framework)
# and yyjson (JSON), both pinned as git submodules under third_party/.
#
# Both projects are built with tests/examples/install disabled so they only produce
# the library targets we link: `cmocka` (static) and `yyjson`.

# cmocka is a shared library by default; force a static build for hermetic tests.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# --- cmocka ---------------------------------------------------------------
set(UNIT_TESTING OFF CACHE BOOL "" FORCE) # do not build cmocka's own tests
set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE) # do not build cmocka's examples
set(WITH_CMOCKERY_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory(cmocka EXCLUDE_FROM_ALL)

# --- yyjson ---------------------------------------------------------------
set(YYJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YYJSON_BUILD_FUZZER OFF CACHE BOOL "" FORCE)
set(YYJSON_BUILD_MISC OFF CACHE BOOL "" FORCE)
set(YYJSON_BUILD_DOC OFF CACHE BOOL "" FORCE)
set(YYJSON_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(yyjson EXCLUDE_FROM_ALL)

# Mark the vendored headers as SYSTEM includes so our -Werror cannot fire on
# third-party code. This does NOT weaken warnings on any of our own sources.
foreach(_nm_vendored cmocka yyjson)
    get_target_property(_nm_vendored_incs ${_nm_vendored} INTERFACE_INCLUDE_DIRECTORIES)
    if(_nm_vendored_incs)
        target_include_directories(${_nm_vendored} SYSTEM INTERFACE ${_nm_vendored_incs})
    endif()
endforeach()
unset(_nm_vendored)
unset(_nm_vendored_incs)
