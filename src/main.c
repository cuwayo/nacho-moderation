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

#include "api_methods.h"
#include "cli.h"
#include "commands.h"
#include "config.h"
#include "metrics.h"
#include "tables.h"
#include "tables_json.h"
#include "tg/api_pool.h"
#include "tg/db.h"
#include "tg/expiry.h"
#include "tg/loop.h"
#include "tg/net_conn.h"
#include "tg/perms.h"
#include "tg/polling.h"
#include "tg/router.h"
#include "tg/settings.h"
#include "tg/tg_api.h"
#include "tg/tg_types.h"
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
 * @brief Ignore an API result (fire-and-forget startup call such as setMyCommands).
 *
 * @param[in] res API result.
 * @param[in] ctx Unused.
 */
static void on_ignore_api(const api_result_t *res, void *ctx)
{
    (void)ctx;
    if (res->status != 0) {
        util_log(LOG_DEBUG, "main", "background API call failed: %d", res->status);
    }
}

/**
 * @brief tg_updates_parse visitor that routes one update (adapter for the seam).
 *
 * @param[in] upd Parsed update.
 * @param[in] ctx The @ref router_t.
 *
 * @return Always 0 (keep iterating the batch).
 */
static int route_update(const tg_update_t *upd, void *ctx)
{
    (void)router_handle(ctx, upd);
    return 0;
}

/**
 * @brief Router/poller batch callback: parse the batch and route every update.
 *
 * @param[in] batch Batch summary (carries the raw reply body).
 * @param[in] ctx   The @ref router_t.
 */
static void on_batch(const poll_batch_t *batch, void *ctx)
{
    router_t *router = ctx;
    if (batch->body == NULL || batch->count == 0) {
        return;
    }
    /* tg_updates_parse keeps yyjson inside tg_types; the visitor routes each
     * update through the permission engine. */
    int rc = tg_updates_parse(batch->body, batch->body_len, route_update, router, NULL);
    if (rc != 0) {
        util_log(LOG_WARN, "main", "failed to parse update batch: %d", rc);
    }
}

/**
 * @brief Poll getUpdates until a signal arrives, routing updates (SPEC section 4.1).
 *
 * @param[in] loop   Loop to run.
 * @param[in] api    API client to pump.
 * @param[in] cfg    Loaded configuration values.
 * @param[in] db     DB worker to pump.
 * @param[in] router Router that consumes updates.
 */
static void run_polling(loop_t *loop, tg_api_t *api, const config_values_t *cfg, db_t *db,
                        router_t *router)
{
    poller_config_t pcfg = {
        .ops = tg_api_ops(api),
        .clock = &util_clock_ops_real,
        .timeout_s = (unsigned)config_get_int(cfg, CFG_POLLING_TIMEOUT),
        .on_batch = on_batch,
        .ctx = router,
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
        db_pump(db);
        int rc = loop_run_once(loop);
        if (rc < 0) {
            util_log(LOG_ERROR, "main", "loop error: %d", rc);
            break;
        }
    }
    /* Shutdown: stop issuing polls, then drop in-flight requests. Full drain of
     * in-flight requests and DB jobs is completed in M7. */
    poller_stop(poller);
    poller_destroy(poller);
    util_log(LOG_INFO, "main", "stopping");
}

/**
 * @brief Probe the Bot API with getMe and return the parsed identity.
 *
 * @param[in]  loop Loop to run.
 * @param[in]  api  API client.
 * @param[out] me   Receives the bot identity on success.
 *
 * @return 0 on success, 1 if interrupted by a signal, 2 on a probe failure.
 */
static int probe_identity(loop_t *loop, tg_api_t *api, tg_me_t *me)
{
    run_state_t rs = {0};
    int rc = tg_api_get_me(api, on_get_me, &rs);
    if (rc != 0) {
        return 2;
    }
    util_log(LOG_INFO, "main", "connecting to the Bot API");
    (void)pump_until_done(loop, api, &rs);

    if (rs.done && rs.status == 0) {
        *me = rs.me;
        (void)printf("getMe ok: id=%lld username=%s name=%s\n", (long long)me->id, me->username,
                     me->first_name);
        /* Flush so a parent reading our stdout sees the probe result even though we
         * then poll indefinitely (stdout is block-buffered on a pipe). */
        (void)fflush(stdout);
        return 0;
    }
    return g_stop ? 1 : 2;
}

/**
 * @brief Bring up the DB worker, permission engine and router, then poll.
 *
 * @param[in] loop Loop to run.
 * @param[in] api  API client.
 * @param[in] cfg  Loaded configuration values.
 * @param[in] me   Bot identity from the getMe probe.
 *
 * @return Process exit code (0 success, 1 runtime error).
 */
static int run_services(loop_t *loop, tg_api_t *api, const config_values_t *cfg, const tg_me_t *me)
{
    db_config_t dcfg = {.path = config_get_str(cfg, CFG_DB_PATH)};
    db_t *db = db_create(&dcfg);
    if (db == NULL || db_arm(db, loop_ops(loop)) != 0) {
        util_log(LOG_ERROR, "main", "failed to open the database");
        db_destroy(db);
        return 1;
    }

    int64_t sudo_ids[256];
    size_t sudo_count = 0;
    (void)config_get_i64_list(cfg, CFG_SUDO_IDS, sudo_ids, sizeof(sudo_ids) / sizeof(sudo_ids[0]),
                              &sudo_count);
    perms_config_t pcfg = {.clock = &util_clock_ops_real,
                           .api = tg_api_ops(api),
                           .bot_id = me->id,
                           .sudo_ids = sudo_ids,
                           .sudo_count = sudo_count};
    perms_t *perms = perms_create(&pcfg);
    if (perms == NULL) {
        util_log(LOG_ERROR, "main", "failed to create the permission engine");
        db_destroy(db);
        return 1;
    }

    /* Per-chat settings cache in front of the DB worker (SPEC section 4.4). */
    settings_config_t scfg = {.db = db_ops(db), .clock = &util_clock_ops_real};
    settings_t *settings = settings_create(&scfg);
    if (settings == NULL) {
        util_log(LOG_ERROR, "main", "failed to create the settings service");
        perms_destroy(perms);
        db_destroy(db);
        return 1;
    }

    router_config_t rcfg = {.api = tg_api_ops(api),
                            .db = db_ops(db),
                            .perms = perms,
                            .settings = settings,
                            .clock = &util_clock_ops_real,
                            .wall = &util_wall_clock_real,
                            .bot_id = me->id,
                            .bot_username = me->username};
    router_t *router = router_create(&rcfg);
    if (router == NULL) {
        util_log(LOG_ERROR, "main", "failed to create the router");
        settings_destroy(settings);
        perms_destroy(perms);
        db_destroy(db);
        return 1;
    }

    /* Lift expired temp bans/mutes every 30 s (SPEC section 5.6). */
    expiry_config_t ecfg = {.ops = loop_ops(loop),
                            .db = db_ops(db),
                            .api = tg_api_ops(api),
                            .wall = &util_wall_clock_real};
    expiry_t *expiry = expiry_create(&ecfg);
    if (expiry == NULL || expiry_start(expiry) != 0) {
        util_log(LOG_WARN, "main", "expiry sweeper unavailable");
    }

    /* Register the command menu with Telegram (best effort). */
    char smc[4096];
    size_t smc_len = 0;
    if (commands_build_set_my_commands(smc, sizeof(smc), &smc_len) == 0) {
        (void)tg_api_request(api, METHOD_SET_MY_COMMANDS, smc, smc_len, 0, 0, on_ignore_api, NULL);
    }

    run_polling(loop, api, cfg, db, router);

    expiry_stop(expiry);
    expiry_destroy(expiry);
    router_destroy(router);
    settings_destroy(settings);
    perms_destroy(perms);
    db_destroy(db);
    return 0;
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

    tg_me_t me = {0};
    int probe = probe_identity(loop, api, &me);
    if (probe != 0) {
        if (probe == 1) {
            util_log(LOG_INFO, "main", "interrupted");
        } else {
            util_log(LOG_ERROR, "main", "getMe probe failed");
        }
        tg_api_destroy(api);
        loop_destroy(loop);
        return 1;
    }

    int exit_code = run_services(loop, api, &cfg, &me);
    tg_api_destroy(api);
    loop_destroy(loop);
    return exit_code;
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
