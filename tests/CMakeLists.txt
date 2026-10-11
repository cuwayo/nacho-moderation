# Test tree (SPEC-ADDENDUM-TOOLCHAIN A2, A5; SPEC-ADDENDUM-SSOT S7).
add_subdirectory(unit)

if(NM_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()

# --- Docs / SSOT consistency tests (S7) ------------------------------------
find_package(Python3 COMPONENTS Interpreter)

if(Python3_Interpreter_FOUND)
    # Golden --help: docs and the binary cannot drift (C8).
    add_test(
        NAME golden-help
        COMMAND
            ${CMAKE_COMMAND} -DNM_TGBOT=$<TARGET_FILE:tgbot>
            "-DNM_GOLDEN=${CMAKE_SOURCE_DIR}/tests/golden/help.txt" -P
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_help.cmake")

    # SSOT consistency checks C1-C5 against the freshly built binary (S7).
    add_test(
        NAME consistency
        COMMAND
            ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/scripts/check_consistency.py" --bin
            $<TARGET_FILE:tgbot> --root "${CMAKE_SOURCE_DIR}")
    set_tests_properties(consistency PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # The check script's own unit tests (S7 script-quality requirement).
    add_test(
        NAME scripts-tests
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests/scripts -p "test_*.py")
    set_tests_properties(
        scripts-tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                              ENVIRONMENT "NM_TGBOT_BIN=$<TARGET_FILE:tgbot>")

    # Integration: drive the real loop against the mock Telegram server (A5.3).
    add_test(
        NAME integration-getme
        COMMAND ${Python3_EXECUTABLE} tests/integration/test_integration_getme.py)
    set_tests_properties(
        integration-getme PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                                 ENVIRONMENT "NM_TGBOT_BIN=$<TARGET_FILE:tgbot>")

    add_test(
        NAME integration-polling
        COMMAND ${Python3_EXECUTABLE} tests/integration/test_integration_polling.py)
    set_tests_properties(
        integration-polling PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                                   ENVIRONMENT "NM_TGBOT_BIN=$<TARGET_FILE:tgbot>")

    # M3: update parsing -> router -> perms -> reply (A5.3).
    add_test(
        NAME integration-router
        COMMAND ${Python3_EXECUTABLE} tests/integration/test_integration_router.py)
    set_tests_properties(
        integration-router PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                                  ENVIRONMENT "NM_TGBOT_BIN=$<TARGET_FILE:tgbot>")

    # M4: moderation commands (ban/kick) against the mock server (A5.3).
    add_test(
        NAME integration-moderation
        COMMAND ${Python3_EXECUTABLE} tests/integration/test_integration_moderation.py)
    set_tests_properties(
        integration-moderation PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                                      ENVIRONMENT "NM_TGBOT_BIN=$<TARGET_FILE:tgbot>")

    # M5: warns, welcome delivery, and the settings panel (A5.3).
    add_test(
        NAME integration-m5
        COMMAND ${Python3_EXECUTABLE} tests/integration/test_integration_m5.py)
    set_tests_properties(
        integration-m5 PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                                ENVIRONMENT "NM_TGBOT_BIN=$<TARGET_FILE:tgbot>")

    # M6: automod (locks, antiflood) and the join captcha (A5.3).
    add_test(
        NAME integration-m6
        COMMAND ${Python3_EXECUTABLE} tests/integration/test_integration_m6.py)
    set_tests_properties(
        integration-m6 PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                                ENVIRONMENT "NM_TGBOT_BIN=$<TARGET_FILE:tgbot>")

    # M7: webhook mode and graceful shutdown (A5.3).
    add_test(
        NAME integration-webhook
        COMMAND ${Python3_EXECUTABLE} tests/integration/test_integration_webhook.py)
    set_tests_properties(
        integration-webhook PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                                     ENVIRONMENT "NM_TGBOT_BIN=$<TARGET_FILE:tgbot>")
else()
    message(STATUS "Python3 interpreter not found; docs/SSOT ctest entries disabled")
endif()
