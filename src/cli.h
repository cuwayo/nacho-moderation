/* cli.h - command-line parsing and the introspection dump flags.
 *
 * SPEC-ADDENDUM-SSOT S5. The dump flags (--dump-*-json, --dump-config-example) and
 * --help are handled before any config or token access, so they need no network and
 * no secrets. They are intentionally omitted from --help (S5) and their output is
 * deterministic.
 *
 * @file cli.h
 * @brief Argument parsing for the tgbot executable.
 * @ingroup util
 */
#ifndef NM_CLI_H
#define NM_CLI_H

#include <stdio.h>

/** @brief What the process should do after parsing arguments. */
typedef enum {
    CLI_ACTION_RUN = 0,             /**< Start the bot (event loop). */
    CLI_ACTION_HELP,                /**< Print --help and exit 0. */
    CLI_ACTION_VERSION,             /**< Print --version and exit 0. */
    CLI_ACTION_CHECK_CONFIG,        /**< Validate a config file and exit. */
    CLI_ACTION_DUMP_COMMANDS_JSON,  /**< Print commands JSON and exit 0. */
    CLI_ACTION_DUMP_CONFIG_JSON,    /**< Print config JSON and exit 0. */
    CLI_ACTION_DUMP_TABLES_JSON,    /**< Print all tables JSON and exit 0. */
    CLI_ACTION_DUMP_CONFIG_EXAMPLE, /**< Print the sample config and exit 0. */
    CLI_ACTION_SELF_TEST            /**< Run an internal loop/timer self-test and exit. */
} cli_action_t;

/** @brief Parsed command-line options. */
typedef struct {
    cli_action_t action;     /**< Selected action. */
    const char *config_path; /**< --config value, or NULL for the default. */
    const char *check_path;  /**< --check-config value (valid when action is CHECK_CONFIG). */
    const char *log_level;   /**< --log-level override, or NULL. */
    int exit_code;           /**< Exit code the caller should use (0 or an errno). */
} cli_options_t;

/**
 * @brief Parse @p argv into @p opts.
 *
 * Recognizes --help/-h, --version/-V, --config <file>, --check-config <file>,
 * --log-level <level>, and the hidden dump flags. On an unknown or malformed
 * argument it prints a diagnostic to stderr and returns -EINVAL.
 *
 * @param[in]  argc Argument count.
 * @param[in]  argv Argument vector (argv[0] is the program name).
 * @param[out] opts Receives the parsed options.
 *
 * @return 0 on success, negative errno on failure.
 * @ownership @p opts holds pointers into @p argv; they live as long as @p argv.
 * @thread_safety Not thread-safe.
 */
int cli_parse(int argc, char **argv, cli_options_t *opts);

/**
 * @brief Print the human-readable --help text to @p out.
 *
 * The text is golden-file tested (tests/golden/help.txt) and reproduced verbatim
 * in docs/building.md, so docs and the binary cannot drift (C8).
 *
 * @param[in] out Destination stream (not closed).
 *
 * @thread_safety Safe; writes only to @p out.
 */
void cli_print_help(FILE *out);

#endif /* NM_CLI_H */
