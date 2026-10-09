# Fuzz targets (SPEC-ADDENDUM-TOOLCHAIN A5.4). Clang + libFuzzer only.
# Build with: cmake --preset fuzz && cmake --build --preset fuzz
file(GLOB NM_FUZZ_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fuzz_*.c")

foreach(_src ${NM_FUZZ_SOURCES})
    get_filename_component(_name ${_src} NAME_WE)
    add_executable(${_name} ${_src})
    target_compile_options(${_name} PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(${_name} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(${_name} PRIVATE libtgbot_core nm_warnings)
endforeach()
