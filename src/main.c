/**
 * @file main.c
 * @brief Executable entry point: argument parsing and top-level dispatch.
 *
 * M1 adds the runtime path: load the configuration, then run one getMe through
 * the io_uring event loop (SPEC section 12). M7 adds signal handling through a
 * signalfd read on the ring, webhook mode, and the graceful-shutdown drain.
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
#include "tg/automod.h"
#include "tg/captcha.h"
#include "tg/db.h"
#include "tg/expiry.h"
#include "tg/loop.h"
#include "tg/net_conn.h"
#include "tg/perms.h"
#include "tg/polling.h"
#include "tg/router.h"
#include "tg/settings.h"
#include "tg/signals.h"
#include "tg/tg_api.h"
#include "tg/tg_types.h"
#include "tg/util.h"
#include "tg/version.h"
#include "tg/webhook.h"

static volatile sig_atomic_t g_stop;         /**< Set by a termination signal. */
static volatile sig_atomic_t g_dump_metrics; /**< Set by SIGUSR1 to dump counters. */

/** @brief Graceful-shutdown drain budget in milliseconds (SPEC section 9). */
#define SHUTDOWN_DRAIN_MS 10000u

/**
 * @brief signalfd callback: record the stop or metrics-dump request.
 *
 * Runs on the loop thread when the signalfd read completes (SPEC section 8).
 *
 * @param[in] signo Signal number.
 * @param[in] ctx   Unused.
 */
static void on_signal(int signo, void *ctx)
{
    (void)ctx;
    if (signo == SIGUSR1) {
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
 * @brief Drain in-flight API requests and DB jobs, bounded by the shutdown budget.
 *
 * Runs the loop for up to @ref SHUTDOWN_DRAIN_MS, pumping the API client and DB
 * worker until both are idle (SPEC section 9). The dedicated polling connection
 * is excluded: a getUpdates long-poll is the update source, which shutdown stops
 * rather than drains. In-flight webhook connections are drained too, so a
 * response that is mid-send is delivered rather than reset. A hard cap keeps
 * shutdown bounded when a peer is unresponsive.
 *
 * @param[in] loop    Loop to run.
 * @param[in] api     API client to drain.
 * @param[in] db      DB worker to drain.
 * @param[in] webhook Webhook server whose connections to drain (may be NULL).
 */
static void drain_in_flight(loop_t *loop, tg_api_t *api, db_t *db, const webhook_t *webhook)
{
    /* "Outstanding" for shutdown excludes the long-poll, which may be mid-flight
     * for up to polling.timeout seconds. */
    size_t shared = tg_api_outstanding(api) - tg_api_outstanding_polling(api);
    uint64_t deadline = clock_now_ms(&util_clock_ops_real) + SHUTDOWN_DRAIN_MS;
    while ((shared > 0 || db_pending(db) > 0 || webhook_active_conns(webhook) > 0) &&
           clock_now_ms(&util_clock_ops_real) < deadline) {
        tg_api_pump(api);
        db_pump(db);
        /* Wait with a short bound so a missed eventfd wakeup still lets db_pump
         * run, and a long timer cannot block past the drain budget. */
        uint64_t now = clock_now_ms(&util_clock_ops_real);
        uint64_t slice = now < deadline ? deadline - now : 0;
        if (slice > 100u) {
            slice = 100u;
        }
        if (loop_run_once_timeout(loop, slice) < 0) {
            break;
        }
        shared = tg_api_outstanding(api) - tg_api_outstanding_polling(api);
    }
    size_t db_left = db_pending(db);
    if (shared > 0 || db_left > 0) {
        util_log(LOG_WARN, "main", "shutdown drain timed out: %zu API request(s), %zu DB job(s)",
                 shared, db_left);
    } else {
        util_log(LOG_INFO, "main", "shutdown drain complete");
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
    /* Stop issuing polls, then drain shared requests and DB jobs. The poller is
     * kept alive across the drain so a late long-poll completion (whose ctx is
     * the poller) is not a use-after-free; it is destroyed afterwards. */
    poller_stop(poller);
    drain_in_flight(loop, api, db, NULL);
    poller_destroy(poller);
}

/**
 * @brief Webhook update callback: parse one Update and route it (SPEC section 4.1).
 *
 * The body is the raw request body of one webhook POST. It is a single Update
 * object, not a batch, so it is parsed with @ref tg_update_parse and handed to the
 * router.
 *
 * @param[in] body Raw request body.
 * @param[in] len  Length of @p body.
 * @param[in] ctx  The @ref router_t.
 */
static void on_webhook_update(const char *body, size_t len, void *ctx)
{
    if (body == NULL || len == 0) {
        return;
    }
    tg_update_t upd;
    if (tg_update_parse(body, len, &upd) == 0) {
        router_t *router = ctx;
        (void)router_handle(router, &upd);
    } else {
        util_log(LOG_WARN, "main", "failed to parse webhook update");
    }
}

/**
 * @brief Serve inbound webhook updates until a signal arrives (SPEC section 4.1).
 *
 * @param[in] loop   Loop to run.
 * @param[in] api    API client to pump.
 * @param[in] cfg    Loaded configuration values.
 * @param[in] db     DB worker to pump.
 * @param[in] router Router that consumes updates.
 */
static void run_webhook(loop_t *loop, tg_api_t *api, const config_values_t *cfg, db_t *db,
                        router_t *router)
{
    webhook_config_t wcfg = {
        .ops = loop_ops(loop),
        .net = &net_ops_real,
        .listen_addr = config_get_str(cfg, CFG_WEBHOOK_LISTEN),
        .secret = config_get_str(cfg, CFG_WEBHOOK_SECRET),
        .max_body = 0,
        .max_conns = 0,
        .on_update = on_webhook_update,
        .ctx = router,
    };
    webhook_t *webhook = webhook_create(&wcfg);
    if (webhook == NULL || webhook_start(webhook) != 0) {
        util_log(LOG_ERROR, "main", "failed to start the webhook server on %s",
                 config_get_str(cfg, CFG_WEBHOOK_LISTEN));
        webhook_destroy(webhook);
        return;
    }
    util_log(LOG_INFO, "main", "listening for webhook updates on %s",
             config_get_str(cfg, CFG_WEBHOOK_LISTEN));

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
    /* Stop accepting, drain in-flight API requests, DB jobs and connection
     * responses, then release the server. The webhook server is kept alive
     * across the drain so its connection completions are not a use-after-free. */
    webhook_stop(webhook);
    drain_in_flight(loop, api, db, webhook);
    webhook_destroy(webhook);
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

    /* Automated moderation (locks, blacklist, antiflood) and join captcha
     * (SPEC sections 5.3, 5.4). Both are wired into the router below. */
    automod_config_t amcfg = {.api = tg_api_ops(api),
                              .db = db_ops(db),
                              .settings = settings,
                              .perms = perms,
                              .clock = &util_clock_ops_real,
                              .wall = &util_wall_clock_real,
                              .bot_id = me->id};
    automod_t *automod = automod_create(&amcfg);

    captcha_config_t ccfg = {.ops = loop_ops(loop),
                             .api = tg_api_ops(api),
                             .db = db_ops(db),
                             .settings = settings,
                             .wall = &util_wall_clock_real,
                             .random = &util_random_ops_real,
                             .on_welcome = router_on_captcha_welcome,
                             .on_welcome_ctx = NULL /* set once the router exists */};
    captcha_t *captcha = captcha_create(&ccfg);

    router_config_t rcfg = {.api = tg_api_ops(api),
                            .db = db_ops(db),
                            .perms = perms,
                            .settings = settings,
                            .automod = automod,
                            .captcha = captcha,
                            .clock = &util_clock_ops_real,
                            .wall = &util_wall_clock_real,
                            .bot_id = me->id,
                            .bot_username = me->username};
    router_t *router = router_create(&rcfg);
    if (router == NULL) {
        util_log(LOG_ERROR, "main", "failed to create the router");
        captcha_destroy(captcha);
        automod_destroy(automod);
        settings_destroy(settings);
        perms_destroy(perms);
        db_destroy(db);
        return 1;
    }
    /* The captcha welcome callback needs the router; wire it now that both
     * exist. The captcha service copies the config, so update its view. */
    if (captcha != NULL) {
        captcha_set_welcome_ctx(captcha, router);
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
    /* Kick members that fail the captcha (SPEC section 5.4). */
    if (captcha == NULL || captcha_start(captcha) != 0) {
        util_log(LOG_WARN, "main", "captcha sweeper unavailable");
    }

    /* Register the command menu with Telegram (best effort). */
    char smc[4096];
    size_t smc_len = 0;
    if (commands_build_set_my_commands(smc, sizeof(smc), &smc_len) == 0) {
        (void)tg_api_request(api, METHOD_SET_MY_COMMANDS, smc, smc_len, 0, 0, on_ignore_api, NULL);
    }

    const char *mode = config_get_str(cfg, CFG_MODE);
    if (strcmp(mode, "webhook") == 0) {
        run_webhook(loop, api, cfg, db, router);
    } else {
        run_polling(loop, api, cfg, db, router);
    }
    /* Each mode function drains in-flight requests and DB jobs (SPEC section 9)
     * before releasing its update source, so nothing is left to drain here. */

    captcha_stop(captcha);
    captcha_destroy(captcha);
    expiry_stop(expiry);
    expiry_destroy(expiry);
    automod_destroy(automod);
    router_destroy(router);
    settings_destroy(settings);
    perms_destroy(perms);
    db_destroy(db);
    util_log(LOG_INFO, "main", "stopped");
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
    /* Required keys and mode-dependent requirements (e.g. a webhook secret of at
     * least 16 characters) are enforced here, before any network or listener. */
    char verr[256] = "";
    if (config_validate(&cfg, verr, sizeof(verr)) != 0) {
        (void)fprintf(stderr, "tgbot: %s\n", verr[0] != '\0' ? verr : "invalid configuration");
        return 2;
    }
    apply_log_level(opts, &cfg);

    /* Seed the captcha nonce PRNG once, before the loop (no blocking entropy
     * read on the event loop; SPEC-ADDENDUM-TOOLCHAIN A3). */
    util_random_seed();

    loop_t *loop = loop_create(256);
    if (loop == NULL) {
        (void)fprintf(stderr, "tgbot: failed to initialize io_uring\n");
        return 1;
    }

    /* Signal handling via signalfd read through the ring (SPEC section 8): a
     * SIGINT/SIGTERM produces a CQE and wakes the loop, unlike an async handler
     * that only sets a flag (io_uring_wait_cqe restarts itself on EINTR). */
    signals_config_t sigcfg = {.ops = loop_ops(loop), .on_signal = on_signal, .ctx = NULL};
    signals_t *signals = signals_create(&sigcfg);
    if (signals == NULL || signals_start(signals) != 0) {
        (void)fprintf(stderr, "tgbot: failed to install signal handling\n");
        signals_destroy(signals);
        loop_destroy(loop);
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
        signals_destroy(signals);
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
        signals_destroy(signals);
        loop_destroy(loop);
        return 1;
    }

    int exit_code = run_services(loop, api, &cfg, &me);
    signals_destroy(signals);
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
