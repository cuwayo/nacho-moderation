/**
 * @file main.c
 * @brief Executable entry point: argument parsing and top-level dispatch.
 *
 * M1 adds the runtime path: load the configuration, then run one getMe through
 * the io_uring event loop (SPEC section 12). Signal handling is a self-eventfd
 * for now; signalfd-via-io_uring is deferred to M7.
 *
 * @ingroup util
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cli.h"
#include "config.h"
#include "tables.h"
#include "tables_json.h"
#include "tg/loop.h"
#include "tg/net_conn.h"
#include "tg/tg_api.h"
#include "tg/util.h"
#include "tg/version.h"

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* Result of the getMe run. */
typedef struct {
    int done;
    int status;
    tg_me_t me;
} run_state_t;

static void on_get_me(int status, const tg_me_t *me, void *ctx)
{
    run_state_t *rs = ctx;
    rs->done = 1;
    rs->status = status;
    if (me != NULL) {
        rs->me = *me;
    }
}

/* Apply the log level from the CLI flag or config. */
static void apply_log_level(const cli_options_t *opts, const config_values_t *cfg)
{
    const char *level =
        opts->log_level != NULL ? opts->log_level : config_get_str(cfg, CFG_LOG_LEVEL);
    if (strcmp(level, "debug") == 0) {
        util_log_set_level(LOG_DEBUG);
    } else if (strcmp(level, "warn") == 0) {
        util_log_set_level(LOG_WARN);
    } else if (strcmp(level, "error") == 0) {
        util_log_set_level(LOG_ERROR);
    } else {
        util_log_set_level(LOG_INFO);
    }
}

/* Pump the loop until getMe finishes or a signal arrives. The callback mutates
 * *rs through its own ctx pointer, so this function only reads it. */
static int pump_until_done(loop_t *loop, const run_state_t *rs)
{
    while (!rs->done && !g_stop) {
        int rc = loop_run_once(loop);
        if (rc < 0) {
            util_log(LOG_ERROR, "main", "loop error: %d", rc);
            return rc;
        }
    }
    return 0;
}

/**
 * @brief Load the config and perform a single getMe through the event loop.
 *
 * @param[in] opts Parsed CLI options.
 *
 * @return Process exit code (0 success, 1 runtime error, 2 usage/config error).
 */
static int run_bot(const cli_options_t *opts)
{
    config_values_t cfg;
    const char *path = opts->config_path != NULL ? opts->config_path : "./config.toml";
    /* A missing default config file is only an error if explicitly requested. */
    int rc =
        config_load(access(path, F_OK) == 0 || opts->config_path != NULL ? path : NULL, NULL, &cfg);
    if (rc != 0) {
        (void)fprintf(stderr, "tgbot: configuration error\n");
        return 2;
    }
    if (config_get_str(&cfg, CFG_BOT_TOKEN)[0] == '\0') {
        (void)fprintf(stderr, "tgbot: bot_token is required (set it in config or $BOT_TOKEN)\n");
        return 2;
    }
    apply_log_level(opts, &cfg);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);

    loop_t *loop = loop_create(256);
    if (loop == NULL) {
        (void)fprintf(stderr, "tgbot: failed to initialize io_uring\n");
        return 1;
    }

    tg_api_config_t acfg = {
        .token = config_get_str(&cfg, CFG_BOT_TOKEN),
        .base_url = config_get_str(&cfg, CFG_API_BASE),
        .user_agent = "nacho-moderator/" TG_VERSION_STRING,
    };
    tg_api_t *api = tg_api_create(&acfg, loop_ops(loop), &net_ops_real, &util_clock_ops_real);
    if (api == NULL) {
        loop_destroy(loop);
        (void)fprintf(stderr, "tgbot: invalid api configuration\n");
        return 2;
    }

    run_state_t rs = {0};
    rc = tg_api_get_me(api, on_get_me, &rs);
    if (rc != 0) {
        tg_api_destroy(api);
        loop_destroy(loop);
        (void)fprintf(stderr, "tgbot: failed to start getMe\n");
        return 1;
    }

    util_log(LOG_INFO, "main", "connecting to %s", acfg.base_url);
    (void)pump_until_done(loop, &rs);

    int exit_code = 0;
    if (rs.done && rs.status == 0) {
        (void)printf("getMe ok: id=%lld username=%s name=%s\n", (long long)rs.me.id, rs.me.username,
                     rs.me.first_name);
    } else if (g_stop) {
        util_log(LOG_INFO, "main", "interrupted");
        exit_code = 1;
    } else {
        util_log(LOG_ERROR, "main", "getMe failed: %d", rs.status);
        exit_code = 1;
    }

    tg_api_destroy(api);
    loop_destroy(loop);
    return exit_code;
}

/* Set to 1 by the self-test timer callback. */
static int g_selftest_fired;

static void on_selftest_timer(struct op *op, int res)
{
    (void)res;
    (void)op;
    g_selftest_fired = 1;
}

/**
 * @brief Run the internal loop self-test (used by check_blocking.sh).
 *
 * Exercises the ring with a standalone timer and no network or token, printing a
 * `loop: ready` sentinel to stderr once the loop is in steady state (the point
 * from which check_blocking.sh begins tracing the main thread).
 *
 * @return Process exit code (0 on success).
 */
static int run_self_test(void)
{
    loop_t *loop = loop_create(64);
    if (loop == NULL) {
        (void)fprintf(stderr, "tgbot: self-test: failed to initialize io_uring\n");
        return 1;
    }
    const loop_ops_t *ops = loop_ops(loop);
    op_t timer = {.type = OP_TIMEOUT, .owner = NULL, .on_complete = on_selftest_timer, .fd = -1};
    if (ops->prep_timeout(ops->ctx, &timer, 1) != 0) {
        loop_destroy(loop);
        return 1;
    }

    (void)fprintf(stderr, "loop: ready\n");
    (void)fflush(stderr);

    int exit_code = 0;
    for (int i = 0; i < 100 && !g_selftest_fired; ++i) {
        if (loop_run_once(loop) < 0) {
            exit_code = 1;
            break;
        }
    }
    loop_destroy(loop);
    if (g_selftest_fired) {
        (void)printf("self-test ok\n");
    }
    return exit_code;
}
static int dispatch(const cli_options_t *opts)
{
    switch (opts->action) {
    case CLI_ACTION_HELP:
        cli_print_help(stdout);
        return 0;
    case CLI_ACTION_VERSION:
        (void)printf("nacho-moderator %s\n", tg_version());
        return 0;
    case CLI_ACTION_DUMP_COMMANDS_JSON:
        return tables_dump_commands_json(stdout) == 0 ? 0 : 1;
    case CLI_ACTION_DUMP_CONFIG_JSON:
        return tables_dump_config_json(stdout) == 0 ? 0 : 1;
    case CLI_ACTION_DUMP_TABLES_JSON:
        return tables_dump_all_json(stdout) == 0 ? 0 : 1;
    case CLI_ACTION_DUMP_CONFIG_EXAMPLE:
        return config_dump_example(stdout) == 0 ? 0 : 1;
    case CLI_ACTION_CHECK_CONFIG: {
        char err[256] = "";
        if (config_check_file(opts->check_path, err, sizeof(err)) != 0) {
            (void)fprintf(stderr, "tgbot: %s\n", err[0] != '\0' ? err : "invalid configuration");
            return 1;
        }
        return 0;
    }
    case CLI_ACTION_SELF_TEST:
        return run_self_test();
    case CLI_ACTION_RUN:
    default:
        return run_bot(opts);
    }
}

int main(int argc, char **argv)
{
    cli_options_t opts;
    if (cli_parse(argc, argv, &opts) != 0) {
        return 2; /* usage error */
    }
    return dispatch(&opts);
}
