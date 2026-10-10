cmake_minimum_required(VERSION 3.25)

project(
    nacho_moderator
    VERSION 0.0.1
    DESCRIPTION "io_uring-based Telegram admin/moderator bot"
    LANGUAGES C)

# --- Global C settings (SPEC-ADDENDUM-TOOLCHAIN A3) ------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# We compile with -std=c11 (__STRICT_ANSI__), which makes glibc hide POSIX APIs
# such as mkstemp/strdup/write. Request the POSIX.1-2008 interface globally so
# sources do not each need a reserved-identifier macro definition.
add_compile_definitions(_POSIX_C_SOURCE=200809L)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE
        Debug
        CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Warnings)
include(Sanitizers)
include(Hardening)

# --- Third-party dependencies (system, via pkg-config; SPEC section 3) -----
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Liburing REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-8)

# Aggregate interface target carrying every system dependency, so targets only
# need to link `nm_deps`. Vendored yyjson/cmocka are added via third_party/.
add_library(nm_deps INTERFACE)
target_link_libraries(
    nm_deps
    INTERFACE OpenSSL::SSL
              OpenSSL::Crypto
              SQLite3::SQLite3
              Liburing::Liburing
              PkgConfig::PCRE2
              Threads::Threads)

add_subdirectory(third_party)

# --- First-party library + executable (SPEC-ADDENDUM-TOOLCHAIN A3) ---------
add_library(
    libtgbot_core STATIC
    src/version.c
    src/util.c
    src/http.c
    src/loop.c
    src/net_conn.c
    src/tls.c
    src/resolver.c
    src/api_methods.c
    src/rate_limit.c
    src/api_retry.c
    src/metrics.c
    src/api_pool.c
    src/polling.c
    src/tg_types.c
    src/cache.c
    src/db.c
    src/perms.c
    src/settings.c
    src/router.c
    src/tg_api.c
    src/commands.c
    src/cmd_ctx.c
    src/config.c
    src/tables_json.c
    src/cli.c
    src/cmd_help.c
    src/cmd_util.c
    src/cmd_mod.c
    src/cmd_msg.c
    src/cmd_warn.c
    src/cmd_settings.c
    src/cmd_automod.c
    src/automod.c
    src/captcha.c
    src/expiry.c)
target_include_directories(
    libtgbot_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
           "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
    libtgbot_core
    PUBLIC nm_deps yyjson
    PRIVATE nm_warnings nm_sanitize nm_hardening)

add_executable(tgbot src/main.c)
target_link_libraries(
    tgbot
    PRIVATE libtgbot_core nm_warnings nm_sanitize nm_hardening)

# --- Tests -----------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# --- Developer targets (SPEC-ADDENDUM-TOOLCHAIN A3) ------------------------
option(NM_BUILD_FUZZ "Build libFuzzer targets (Clang only)" OFF)

add_custom_target(
    format
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/format.sh" --fix
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "clang-format (in place)")

add_custom_target(
    format-check
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/format.sh" --check
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "clang-format (check)")

add_custom_target(
    tidy
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tidy.sh" "${CMAKE_BINARY_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "clang-tidy")

add_custom_target(
    cppcheck
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/cppcheck.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "cppcheck")

add_custom_target(
    coverage
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/coverage.sh" "${CMAKE_BINARY_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "coverage report (gcovr/lcov)")

# --- Documentation targets (SPEC-ADDENDUM-DOCS D1, S8) ---------------------
# `docs` and `docs-check` are optional: when Doxygen is missing, configure
# succeeds and the targets are skipped with a status message (D1).
find_package(Doxygen QUIET)

if(Doxygen_FOUND)
    set(DOXYGEN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/docs")
    # Local `docs`: warnings are reported but do not fail the build (D1).
    set(DOXYGEN_WARN_AS_ERROR "NO")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in"
                   "${DOXYGEN_OUTPUT_DIR}/Doxyfile" @ONLY)
    # Strict `docs-check`: warnings become errors (D1, D4, D9).
    set(DOXYGEN_WARN_AS_ERROR "FAIL_ON_WARNINGS")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in"
                   "${DOXYGEN_OUTPUT_DIR}/Doxyfile.check" @ONLY)

    add_custom_target(
        docs
        COMMAND "${DOXYGEN_EXECUTABLE}" "${DOXYGEN_OUTPUT_DIR}/Doxyfile"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Doxygen HTML -> ${DOXYGEN_OUTPUT_DIR}/html"
        VERBATIM)

    add_custom_target(
        docs-check
        COMMAND "${DOXYGEN_EXECUTABLE}" "${DOXYGEN_OUTPUT_DIR}/Doxyfile.check"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Doxygen with warnings as errors"
        VERBATIM)
else()
    message(STATUS "Doxygen not found; 'docs' and 'docs-check' targets disabled (D1)")
    add_custom_target(
        docs
        COMMAND ${CMAKE_COMMAND} -E echo "Doxygen not installed; docs target skipped."
        COMMENT "docs (skipped)")
endif()

# docgen: rewrite generated blocks from the built binary's tables (S6, S8).
add_custom_target(
    docs-update
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/docgen.py" --update --bin
            "$<TARGET_FILE:tgbot>" --root "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS tgbot
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "docgen --update (rewrite generated doc blocks)"
    VERBATIM)

add_custom_target(
    docs-verify
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/docgen.py" --check --bin
            "$<TARGET_FILE:tgbot>" --root "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_consistency.py" --bin
            "$<TARGET_FILE:tgbot>" --root "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS tgbot
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "docgen --check + check_consistency.py"
    VERBATIM)

# Regenerate the committed sample config from the table (S3, C5).
add_custom_target(
    gen-config-example
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/config"
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_config_example.sh"
            "$<TARGET_FILE:tgbot>" "${CMAKE_CURRENT_SOURCE_DIR}/config/config.example.toml"
    DEPENDS tgbot
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "regenerate config/config.example.toml"
    VERBATIM)

# Man pages from scdoc sources (D6), optional when scdoc is absent.
find_program(SCDOC_EXECUTABLE scdoc)
if(SCDOC_EXECUTABLE)
    add_custom_target(
        man
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_man.sh" "${SCDOC_EXECUTABLE}"
                "${CMAKE_BINARY_DIR}/man"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "build man pages with scdoc"
        VERBATIM)
else()
    message(STATUS "scdoc not found; 'man' target disabled")
endif()

# --- Install ---------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS tgbot RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES config/config.example.toml
        DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/nachomoderator")
if(SCDOC_EXECUTABLE)
    install(CODE "execute_process(COMMAND ${SCDOC_EXECUTABLE} INPUT_FILE \
${CMAKE_CURRENT_SOURCE_DIR}/docs/man/tgbot.1.scd OUTPUT_FILE \
${CMAKE_BINARY_DIR}/man/tgbot.1)")
    install(CODE "execute_process(COMMAND ${SCDOC_EXECUTABLE} INPUT_FILE \
${CMAKE_CURRENT_SOURCE_DIR}/docs/man/tgbot.toml.5.scd OUTPUT_FILE \
${CMAKE_BINARY_DIR}/man/tgbot.toml.5)")
    install(FILES "${CMAKE_BINARY_DIR}/man/tgbot.1" DESTINATION "${CMAKE_INSTALL_MANDIR}/man1")
    install(FILES "${CMAKE_BINARY_DIR}/man/tgbot.toml.5"
            DESTINATION "${CMAKE_INSTALL_MANDIR}/man5")
endif()
