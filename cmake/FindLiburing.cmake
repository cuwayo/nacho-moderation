# FindLiburing (SPEC-ADDENDUM-TOOLCHAIN A1, A2).
# Thin pkg-config wrapper that also honours the pkg-config search path, so system
# packages are used rather than a bundled copy.
include_guard(GLOBAL)

find_package(PkgConfig REQUIRED)

pkg_check_modules(PC_LIBURING QUIET liburing)

find_path(
    Liburing_INCLUDE_DIR
    NAMES liburing.h
    HINTS ${PC_LIBURING_INCLUDE_DIRS}
    PATH_SUFFIXES liburing)

find_library(
    Liburing_LIBRARY
    NAMES uring liburing
    HINTS ${PC_LIBURING_LIBRARY_DIRS})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    Liburing
    REQUIRED_VARS Liburing_LIBRARY Liburing_INCLUDE_DIR
    VERSION_VAR PC_LIBURING_VERSION)

if(Liburing_FOUND AND NOT TARGET Liburing::Liburing)
    add_library(Liburing::Liburing UNKNOWN IMPORTED)
    set_target_properties(
        Liburing::Liburing
        PROPERTIES IMPORTED_LOCATION "${Liburing_LIBRARY}"
                   INTERFACE_INCLUDE_DIRECTORIES "${Liburing_INCLUDE_DIR}")
endif()

mark_as_advanced(Liburing_INCLUDE_DIR Liburing_LIBRARY)
