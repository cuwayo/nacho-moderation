# Release hardening (SPEC-ADDENDUM-TOOLCHAIN A3). Applied only in Release builds.
# Provides an INTERFACE target `nm_hardening`.
include_guard(GLOBAL)

add_library(nm_hardening INTERFACE)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(
        nm_hardening
        INTERFACE -D_FORTIFY_SOURCE=3
                  -fstack-protector-strong
                  -fstack-clash-protection
                  -fcf-protection=full
                  -fPIE)

    target_link_options(
        nm_hardening
        INTERFACE -pie
                  -Wl,-z,relro
                  -Wl,-z,now
                  -Wl,-z,noexecstack)
endif()
