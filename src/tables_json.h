/* tables_json.h - deterministic JSON views of the SSOT tables.
 *
 * SPEC-ADDENDUM-SSOT S5. The binary exports its own tables as JSON so scripts
 * never regex-parse C (S1). Output is stable (table order, no timestamps),
 * UTF-8, and newline-terminated. These functions need no network and no token.
 *
 * @file tables_json.h
 * @brief Introspection dumps: --dump-commands-json, --dump-config-json,
 *        --dump-tables-json.
 * @ingroup util
 */
#ifndef NM_TABLES_JSON_H
#define NM_TABLES_JSON_H

#include <stdio.h>

/**
 * @brief Write the commands table as a JSON array to @p out.
 *
 * Each element: {id,name,min_role,user_right,bot_right,flags[],usage,brief,hidden}.
 *
 * @param[in] out Destination stream (not closed).
 *
 * @return 0 on success, negative errno on failure (-ENOMEM).
 * @thread_safety Safe; writes only to @p out.
 */
int tables_dump_commands_json(FILE *out);

/**
 * @brief Write the config schema as a JSON array to @p out.
 *
 * Each element: {id,key,env,type,default,restart,secret,required,enum_values[],brief}.
 *
 * @param[in] out Destination stream (not closed).
 *
 * @return 0 on success, negative errno on failure (-ENOMEM).
 * @thread_safety Safe; writes only to @p out.
 */
int tables_dump_config_json(FILE *out);

/**
 * @brief Write every table as one JSON object to @p out.
 *
 * Keys: commands, config, locks, api_methods, errors, metrics.
 *
 * @param[in] out Destination stream (not closed).
 *
 * @return 0 on success, negative errno on failure (-ENOMEM).
 * @thread_safety Safe; writes only to @p out.
 */
int tables_dump_all_json(FILE *out);

#endif /* NM_TABLES_JSON_H */
