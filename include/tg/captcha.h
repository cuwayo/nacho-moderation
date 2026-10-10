/**
 * @file captcha.h
 * @brief Join verification (captcha) flow (SPEC section 5.4).
 *
 * When a chat enables the captcha, a new member is immediately restricted (no
 * `sendMessage` to the user), sent an inline-keyboard "I'm human" button and
 * recorded in `pending_captcha` with a deadline. A press by the correct user
 * lifts the restriction, deletes the prompt and delivers the welcome; a press by
 * anyone else is rejected (SPEC section 10); when the deadline passes a periodic
 * sweep kicks the member and cleans up (SPEC sections 5.4, 5.6).
 *
 * The nonce in the callback data (`cap:<chat_id>:<user_id>:<nonce>`, at most 64
 * bytes) is drawn from the injectable @ref random_ops_t so tests are
 * deterministic (SPEC-ADDENDUM-TOOLCHAIN A3). The callback parser
 * (@ref captcha_parse_callback) is pure and fuzz-tested.
 *
 * @ingroup automod
 */
#ifndef TG_CAPTCHA_H
#define TG_CAPTCHA_H

#include <stddef.h>
#include <stdint.h>

#include "tg/db.h"
#include "tg/loop.h"
#include "tg/settings.h"
#include "tg/tg_api.h"
#include "tg/tg_types.h"
#include "tg/util.h"

/** @brief Default seconds a member has to solve the captcha (SPEC section 6). */
#define CAPTCHA_DEFAULT_TIMEOUT 120
/** @brief Period of the timeout sweep (ms). */
#define CAPTCHA_SWEEP_INTERVAL_MS 15000u
/** @brief Maximum length of the captcha callback nonce (hex). */
#define CAPTCHA_NONCE_LEN 8u

/**
 * @brief Callback invoked when a member joins and must be greeted.
 *
 * Called when the captcha is disabled (so the caller delivers the welcome
 * immediately) and again after a member passes the challenge. The router wires
 * this to its welcome delivery so the captcha module never needs the welcome
 * template.
 *
 * @param[in] ctx  Caller context (the router).
 * @param[in] chat Chat joined.
 * @param[in] user Joining user.
 */
typedef void (*captcha_welcome_fn)(void *ctx, const tg_chat_t *chat, const tg_user_t *user);

/** @brief Configuration for the captcha service. All pointers must outlive it. */
typedef struct {
    const loop_ops_t *ops;         /**< Loop vtable (arms the timeout sweep timer). */
    const tg_api_ops_t *api;       /**< API seam (restrict / send / delete / kick). */
    const db_ops_t *db;            /**< DB seam (`pending_captcha` rows). */
    const settings_t *settings;    /**< Settings service (captcha policy). */
    const wall_clock_ops_t *wall;  /**< Wall clock (deadlines). */
    const random_ops_t *random;    /**< Random source (the nonce). */
    captcha_welcome_fn on_welcome; /**< Welcome delivery (may be NULL). */
    void *on_welcome_ctx;          /**< Context for @ref on_welcome. */
    uint64_t interval_ms;          /**< Sweep interval (0 = CAPTCHA_SWEEP_INTERVAL_MS). */
} captcha_config_t;

/** @brief The captcha service (opaque; see captcha.c). */
typedef struct captcha captcha_t;

/**
 * @brief Create the captcha service.
 *
 * @param[in] cfg Configuration (copied; pointers must outlive the service).
 *
 * @return A new service, or NULL on failure.
 * @ownership Caller must call captcha_destroy().
 * @thread_safety Loop thread only.
 */
captcha_t *captcha_create(const captcha_config_t *cfg);

/**
 * @brief Destroy the service.
 *
 * @param[in] cap Service (may be NULL).
 * @thread_safety Loop thread only.
 */
void captcha_destroy(captcha_t *cap);

/**
 * @brief Set the context passed to @ref captcha_config_t::on_welcome.
 *
 * The runtime creates the captcha service before the router (the router needs
 * the service), so the welcome context (the router) is supplied afterwards.
 *
 * @param[in] cap Service (may be NULL).
 * @param[in] ctx Welcome-callback context.
 * @thread_safety Loop thread only.
 */
void captcha_set_welcome_ctx(captcha_t *cap, void *ctx);

/**
 * @brief Begin join handling for @p user in @p chat (SPEC section 5.4).
 *
 * Fetches the chat policy. If the captcha is enabled, restricts @p user, sends
 * the "I'm human" prompt and stores the pending row; otherwise it invokes
 * @ref captcha_config_t::on_welcome so the member is greeted as usual.
 *
 * @param[in] cap  Service.
 * @param[in] chat Chat joined.
 * @param[in] user Joining user.
 *
 * @return 0 if the request was started (including "captcha off"), negative errno
 *         on a fatal argument error.
 * @thread_safety Loop thread only.
 */
int captcha_begin(captcha_t *cap, const tg_chat_t *chat, const tg_user_t *user);

/**
 * @brief Handle a callback query if it is a captcha button press.
 *
 * @param[in] cap Service.
 * @param[in] upd Callback-query update.
 *
 * @return 1 if the update was a captcha callback (handled), 0 if it was not a
 *         captcha payload (the caller should try other handlers).
 * @thread_safety Loop thread only.
 */
int captcha_handle_callback(captcha_t *cap, const tg_update_t *upd);

/**
 * @brief Arm the periodic timeout sweep (SPEC section 5.6).
 *
 * @param[in] cap Service.
 *
 * @return 0 if armed, negative errno on failure.
 * @thread_safety Loop thread only.
 */
int captcha_start(captcha_t *cap);

/**
 * @brief Stop the timeout sweep.
 *
 * @param[in] cap Service (may be NULL).
 * @thread_safety Loop thread only.
 */
void captcha_stop(captcha_t *cap);

/**
 * @brief Run one timeout sweep now (kick every due member).
 *
 * Exposed for the unit test and for a manual sweep; the periodic timer calls it.
 *
 * @param[in] cap Service.
 *
 * @return 0 if the query was submitted, negative errno on failure.
 * @thread_safety Loop thread only.
 */
int captcha_sweep_now(captcha_t *cap);

/**
 * @brief Parse a captcha callback payload (pure).
 *
 * The payload is `cap:<chat_id>:<user_id>:<nonce>` with a nonce of 1..64 bytes.
 * The caller enforces the ownership rule by comparing @p out_user with the
 * presser's id (SPEC section 10).
 *
 * @param[in]  data     Callback data (may be NULL).
 * @param[out] out_chat Receives the chat id.
 * @param[out] out_user Receives the challenged user id.
 * @param[out] nonce    Receives the nonce (NUL-terminated).
 * @param[in]  nonce_cap Capacity of @p nonce.
 *
 * @return 0 on a well-formed payload, -EINVAL otherwise (including a too-small
 *         @p nonce buffer).
 * @thread_safety Safe (no shared state).
 */
int captcha_parse_callback(const char *data, int64_t *out_chat, int64_t *out_user, char *nonce,
                           size_t nonce_cap);

#endif /* TG_CAPTCHA_H */
