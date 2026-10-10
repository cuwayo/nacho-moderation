/**
 * @file cmd_settings.h
 * @brief Public helpers of the settings/welcome command group (SPEC 5.2, 5.4).
 *
 * The handlers themselves are declared in `commands.h` (generated from
 * `commands.def`); this header exposes the pure helpers that other modules (and
 * the tests) call directly: the welcome-template renderer and the settings-panel
 * callback parser.
 *
 * @ingroup commands
 */
#ifndef TG_CMD_SETTINGS_H
#define TG_CMD_SETTINGS_H

#include <stddef.h>
#include <stdint.h>

#include "tg/tg_types.h"

/**
 * @brief Render a welcome template, substituting the SPEC section 5.2 placeholders.
 *
 * Placeholders: `{first}` (first name), `{mention}` (an HTML mention link),
 * `{chat}` (chat title) and `{count}` (member count, 0 when unknown). The
 * substituted values are HTML-escaped except `{mention}`, which expands to a
 * ready-made mention link; the surrounding template text is copied verbatim so
 * authors may include markup. An unknown `{...}` token is copied through
 * unchanged.
 *
 * @param[in]  tmpl         Template (may be NULL/empty).
 * @param[in]  user         Joining user (name/username source).
 * @param[in]  chat_title   Chat title (may be NULL).
 * @param[in]  member_count Member count (0 if unknown).
 * @param[out] out          Destination buffer.
 * @param[in]  cap          Capacity of @p out.
 *
 * @return 0 on success, -ENOSPC if @p out is too small, -EINVAL on bad arguments.
 * @ownership @p out is caller-owned.
 * @thread_safety Safe (no shared state).
 */
int cmd_render_welcome(const char *tmpl, const tg_user_t *user, const char *chat_title,
                       int member_count, char *out, size_t cap);

/**
 * @brief Parse a settings-panel callback payload.
 *
 * The payload is `set:<chat_id>:<user_id>:<key>`. This checks the structure; the
 * ownership rule (SPEC section 10) is enforced by the caller comparing
 * @p out_user with the presser's id.
 *
 * @param[in]  data     Callback data (may be NULL).
 * @param[out] out_chat Receives the chat id.
 * @param[out] out_user Receives the owner user id.
 * @param[out] out_key  Receives a pointer to the key within @p data (may be NULL).
 *
 * @return 0 on a well-formed payload, -EINVAL otherwise.
 * @ownership @p *out_key points into @p data; valid as long as @p data is.
 * @thread_safety Safe (no shared state).
 */
int cmd_settings_parse_callback(const char *data, int64_t *out_chat, int64_t *out_user,
                                const char **out_key);

#endif /* TG_CMD_SETTINGS_H */
