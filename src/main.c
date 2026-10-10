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
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cli.h"
#include "config.h"
#include "metrics.h"
#include "tables.h"
#include "tables_json.h"
#include "tg/api_pool.h"
#include "tg/loop.h"
#include "tg/net_conn.h"
#include "tg/polling.h"
#include "tg/tg_api.h"
#include "tg/util.h"
#include "tg/version.h"

static volatile sig_atomic_t g_stop;         /**< Set by a termination signal. */
static volatile sig_atomic_t g_dump_metrics; /**< Set by SIGUSR1 to dump counters. */

/**
 * @brief Signal handler: set the stop flag, or the metrics-dump flag for SIGUSR1.
 *
 * @param[in] sig Signal number.
 */
static void on_signal(int sig)
{
    if (sig == SIGUSR1) {
        g_dump_metrics = 1;
        return;
    }
    g_stop = 1;
}

/** @brief Result of the getMe probe. */
typedef struct {
    volatile int done; /**< Set by on_get_me() when the probe completes. */
    int status;        /**< Probe status (0 on success). */
    tg_me_t me;        /**< Parsed identity (valid when status is 0). */
} run_state_t;

/**
 * @brief getMe completion: record the status and parsed identity.
 *
 * @param[in] res API result.
 * @param[in] ctx The @ref run_state_t to fill.
 */
static void on_get_me(const api_result_t *res, void *ctx)
{
    run_state_t *rs = ctx;
    rs->done = 1;
    rs->status = res->status;
    if (res->status == 0 && res->body != NULL) {
        (void)tg_api_parse_me(res->body, res->body_len, &rs->me);
    }
}

/**
 * @brief Apply the log level from the CLI flag or config.
 *
 * @param[in] opts Parsed CLI options.
 * @param[in] cfg  Loaded configuration values.
 */
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

/**
 * @brief Pump the loop until getMe finishes or a signal arrives.
 *
 * The callback mutates @p rs through its own ctx pointer, so this function only
 * reads it.
 *
 * @param[in] loop Loop to run.
 * @param[in] api  API client to pump.
 * @param[in] rs   Probe state (read-only here).
 *
 * @return 0 on completion, negative errno on a loop error.
 */
static int pump_until_done(loop_t *loop, tg_api_t *api, const run_state_t *rs)
{
    while (!rs->done && !g_stop) {
        tg_api_pump(api);
        if (rs->done) {
            break; /* the callback fired during dispatch; no more I/O to wait for */
        }
        int rc = loop_run_once(loop);
        if (rc < 0) {
            util_log(LOG_ERROR, "main", "loop error: %d", rc);
            return rc;
        }
    }
    return 0;
}

/**
 * @brief Poll getUpdates until a signal arrives (SPEC section 4.1).
 *
 * @param[in] loop Loop to run.
 * @param[in] api  API client to pump.
 * @param[in] cfg  Loaded configuration values.
 */
static void run_polling(loop_t *loop, tg_api_t *api, const config_values_t *cfg)
{
    poller_config_t pcfg = {
        .ops = tg_api_ops(api),
        .clock = &util_clock_ops_real,
        .timeout_s = (unsigned)config_get_int(cfg, CFG_POLLING_TIMEOUT),
        .on_batch = NULL,
        .ctx = NULL,
    };
    poller_t *poller = poller_create(&pcfg);
    if (poller == NULL) {
        util_log(LOG_ERROR, "main", "failed to create poller");
        return;
    }
    if (poller_start(poller) != 0) {
        util_log(LOG_ERROR, "main", "failed to start polling");
        poller_destroy(poller);
        return;
    }

    util_log(LOG_INFO, "main", "polling for updates (SIGINT/SIGTERM to stop)");
    while (!g_stop) {
        if (g_dump_metrics) {
            g_dump_metrics = 0;
            metrics_dump(stderr);
        }
        tg_api_pump(api);
        int rc = loop_run_once(loop);
        if (rc < 0) {
            util_log(LOG_ERROR, "main", "loop error: %d", rc);
            break;
        }
    }
    /* Shutdown: stop issuing polls and drop in-flight requests. Full drain of
     * in-flight requests is completed in M7. */
    poller_stop(poller);
    poller_destroy(poller);
    util_log(LOG_INFO, "main", "stopping (last offset=%lld)", (long long)0);
}

/**
 * @brief Load the config, probe with getMe, then long-poll until signalled.
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
    (void)sigaction(SIGUSR1, &sa, NULL);

    loop_t *loop = loop_create(256);
    if (loop == NULL) {
        (void)fprintf(stderr, "tgbot: failed to initialize io_uring\n");
        return 1;
    }

    tg_api_config_t acfg = {
        .token = config_get_str(&cfg, CFG_BOT_TOKEN),
        .base_url = config_get_str(&cfg, CFG_API_BASE),
        .user_agent = "nacho-moderator/" TG_VERSION_STRING,
        .pool_size = (unsigned)config_get_int(&cfg, CFG_API_POOL_SIZE),
        .request_timeout_ms = (uint64_t)config_get_int(&cfg, CFG_API_REQUEST_TIMEOUT_MS),
    };
    tg_api_t *api = tg_api_create(&acfg, loop_ops(loop), &net_ops_real, &util_clock_ops_real);
    if (api == NULL) {
        loop_destroy(loop);
        (void)fprintf(stderr, "tgbot: invalid api configuration\n");
        return 2;
    }

    /* Probe with getMe: validates the token and logs the bot identity. */
    run_state_t rs = {0};
    rc = tg_api_get_me(api, on_get_me, &rs);
    if (rc != 0) {
        tg_api_destroy(api);
        loop_destroy(loop);
        (void)fprintf(stderr, "tgbot: failed to start getMe\n");
        return 1;
    }
    util_log(LOG_INFO, "main", "connecting to %s", acfg.base_url);
    (void)pump_until_done(loop, api, &rs);

    if (rs.done && rs.status == 0) {
        (void)printf("getMe ok: id=%lld username=%s name=%s\n", (long long)rs.me.id, rs.me.username,
                     rs.me.first_name);
        /* Flush so a parent process reading our stdout sees the probe result even
         * though we then poll indefinitely (stdout is block-buffered on a pipe). */
        (void)fflush(stdout);
    } else if (g_stop) {
        util_log(LOG_INFO, "main", "interrupted");
        tg_api_destroy(api);
        loop_destroy(loop);
        return 1;
    } else {
        util_log(LOG_ERROR, "main", "getMe failed: %d", rs.status);
        tg_api_destroy(api);
        loop_destroy(loop);
        return 1;
    }

    run_polling(loop, api, &cfg);

    tg_api_destroy(api);
    loop_destroy(loop);
    return 0;
}

/** @brief Set to 1 by the self-test timer callback. */
static int g_selftest_fired;

/**
 * @brief Self-test timer completion: mark the timer as fired.
 *
 * @param[in] op  The timer op (unused).
 * @param[in] res Timer result (unused).
 */
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

/**
 * @brief Perform the action selected by the parsed CLI options.
 *
 * @param[in] opts Parsed CLI options.
 *
 * @return Process exit code.
 */
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

/**
 * @brief Program entry point: parse arguments and dispatch.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 *
 * @return Process exit code (0 success, 1 runtime error, 2 usage error).
 */
int main(int argc, char **argv)
{
    cli_options_t opts;
    if (cli_parse(argc, argv, &opts) != 0) {
        return 2; /* usage error */
    }
    return dispatch(&opts);
}
