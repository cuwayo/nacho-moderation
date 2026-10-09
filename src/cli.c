/* cli.c - command-line parsing and the introspection dump flags.
 *
 * SPEC-ADDENDUM-SSOT S5. Kept deliberately dependency-free (no config, no network)
 * so the dump flags work on a fresh checkout with no token. The hidden dump flags
 * are not listed in --help.
 *
 * @file cli.c
 * @brief Argument parsing for the tgbot executable.
 * @ingroup util
 */
#include "cli.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "tg/version.h"

/* Long option spellings, kept as named constants so they appear once. */
static const char OPT_HELP[] = "--help";
static const char OPT_VERSION[] = "--version";
static const char OPT_CONFIG[] = "--config";
static const char OPT_CHECK_CONFIG[] = "--check-config";
static const char OPT_LOG_LEVEL[] = "--log-level";
static const char OPT_DUMP_COMMANDS[] = "--dump-commands-json";
static const char OPT_DUMP_CONFIG[] = "--dump-config-json";
static const char OPT_DUMP_TABLES[] = "--dump-tables-json";
static const char OPT_DUMP_EXAMPLE[] = "--dump-config-example";
static const char OPT_SELF_TEST[] = "--self-test";

static int is_help(const char *a)
{
    return strcmp(a, OPT_HELP) == 0 || strcmp(a, "-h") == 0;
}

static int is_version(const char *a)
{
    return strcmp(a, OPT_VERSION) == 0 || strcmp(a, "-V") == 0;
}

/* Options that set an action and exit immediately (no value follows). */
static int try_terminal_flag(const char *a, cli_options_t *opts)
{
    struct {
        const char *flag;
        cli_action_t action;
    } map[] = {
        {OPT_DUMP_COMMANDS, CLI_ACTION_DUMP_COMMANDS_JSON},
        {OPT_DUMP_CONFIG, CLI_ACTION_DUMP_CONFIG_JSON},
        {OPT_DUMP_TABLES, CLI_ACTION_DUMP_TABLES_JSON},
        {OPT_DUMP_EXAMPLE, CLI_ACTION_DUMP_CONFIG_EXAMPLE},
        {OPT_SELF_TEST, CLI_ACTION_SELF_TEST},
    };
    if (is_help(a)) {
        opts->action = CLI_ACTION_HELP;
        return 1;
    }
    if (is_version(a)) {
        opts->action = CLI_ACTION_VERSION;
        return 1;
    }
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (strcmp(a, map[i].flag) == 0) {
            opts->action = map[i].action;
            return 1;
        }
    }
    return 0;
}

/* Consume a "--opt value" pair; @p i points at the value on success. */
static int try_value_option(int argc, char **argv, int *i, cli_options_t *opts)
{
    const char *a = argv[*i];
    int is_config = strcmp(a, OPT_CONFIG) == 0;
    int is_check = strcmp(a, OPT_CHECK_CONFIG) == 0;
    int is_level = strcmp(a, OPT_LOG_LEVEL) == 0;
    if (!is_config && !is_check && !is_level) {
        return 0;
    }
    if (*i + 1 >= argc) {
        (void)fprintf(stderr, "tgbot: option '%s' requires a value\n", a);
        return -1;
    }
    const char *val = argv[++(*i)];
    if (is_config) {
        opts->config_path = val;
    } else if (is_check) {
        opts->check_path = val;
        opts->action = CLI_ACTION_CHECK_CONFIG;
    } else {
        opts->log_level = val;
    }
    return 1;
}

int cli_parse(int argc, char **argv, cli_options_t *opts)
{
    if (argv == NULL || opts == NULL || argc < 1) {
        return -EINVAL;
    }

    *opts = (cli_options_t){.action = CLI_ACTION_RUN, .exit_code = 0};

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];

        if (try_terminal_flag(a, opts)) {
            return 0;
        }

        int consumed = try_value_option(argc, argv, &i, opts);
        if (consumed < 0) {
            return -EINVAL;
        }
        if (consumed > 0) {
            continue;
        }

        (void)fprintf(stderr, "tgbot: unknown option '%s' (try --help)\n", a);
        return -EINVAL;
    }

    return 0;
}

void cli_print_help(FILE *out)
{
    (void)fprintf(out,
                  "tgbot %s - asynchronous Telegram group admin bot\n"
                  "\n"
                  "Usage:\n"
                  "  tgbot [OPTIONS]\n"
                  "\n"
                  "Options:\n"
                  "  --config <file>       Path to config.toml (default: ./config.toml)\n"
                  "  --check-config <file> Validate a config file and exit\n"
                  "  --log-level <level>   Override log level (error|warn|info|debug)\n"
                  "  -h, --help            Show this help and exit\n"
                  "  -V, --version         Show version and exit\n"
                  "\n"
                  "Exit codes:\n"
                  "  0   success\n"
                  "  1   runtime or configuration error\n"
                  "  2   usage error\n",
                  tg_version());
}
