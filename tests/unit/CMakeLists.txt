# Unit tests (SPEC-ADDENDUM-TOOLCHAIN A5.1). One file per module, named test_*.c.

# Test doubles shared by several tests (SPEC-ADDENDUM-TOOLCHAIN A5.2): the
# fake_ring implements loop_ops_t without liburing.
add_library(nm_test_mock STATIC "${CMAKE_SOURCE_DIR}/tests/mock/fake_ring.c")
target_include_directories(nm_test_mock PUBLIC "${CMAKE_SOURCE_DIR}/tests/mock")
target_link_libraries(nm_test_mock PUBLIC libtgbot_core nm_warnings nm_sanitize)

file(GLOB NM_UNIT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.c")

foreach(_src ${NM_UNIT_TEST_SOURCES})
    get_filename_component(_name ${_src} NAME_WE)
    add_executable(${_name} ${_src})
    target_link_libraries(
        ${_name}
        PRIVATE libtgbot_core
                nm_test_mock
                cmocka
                nm_warnings
                nm_sanitize)
    add_test(NAME ${_name} COMMAND ${_name})
endforeach()
