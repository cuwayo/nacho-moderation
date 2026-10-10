/* config.h - configuration schema and values derived from src/config.def.
 *
 * SPEC-ADDENDUM-SSOT S3. The set of configuration keys lives in exactly one place
 * (src/config.def); this header turns it into the CFG_* enum, the descriptor
 * table and the runtime value struct. Precedence (CLI flag > env var > file >
 * default) is implemented once in config.c, driven by the table.
 *
 * @file config.h
 * @brief Configuration key identifiers, descriptors, and parsed values.
 * @ingroup util
 */
#ifndef NM_CONFIG_H
#define NM_CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "tables.h"

/** @brief Configuration key identifiers, one per config.def row, plus CFG_COUNT. */
typedef enum {
#define K(id, key, env, type, def, restart, secret, brief) CFG_##id,
#define CFG_REQUIRED(id)
#include "config.def"
#undef CFG_REQUIRED
#undef K
    CFG_COUNT /**< Number of configuration keys. */
} cfg_id_t;

/** @brief Static description of one configuration key.
 *
 * Field order groups pointers before the enum/int tail so the struct has no
 * padding (clang-analyzer optin.performance.Padding). */
typedef struct {
    const char *key;   /**< Dotted lowercase config.toml key. */
    const char *env;   /**< Environment variable name. */
    const char *def;   /**< Default as a C literal string ("" = none/required). */
    const char *brief; /**< One-line description. */
    cfg_id_t id;       /**< Stable identifier (CFG_<id>). */
    cfg_type_t type;   /**< Value type. */
    int restart;       /**< 1 if a change requires a restart. */
    int secret;        /**< 1 if the value is redacted in logs and dumps. */
} cfg_desc_t;

/** @brief The configuration schema in config.def order (length CFG_COUNT). */
extern const cfg_desc_t config_table[CFG_COUNT];

/**
 * @brief Report whether a key is required (listed in CFG_REQUIRED).
 *
 * @param[in] id Key identifier.
 *
 * @return 1 if the key must be provided, 0 otherwise.
 * @thread_safety Read-only; safe from any thread.
 */
int config_is_required(cfg_id_t id);

/**
 * @brief Look up a key descriptor by its dotted TOML key.
 *
 * @param[in] key Dotted key, e.g. "api.pool_size".
 *
 * @return Pointer to the descriptor, or NULL if unknown.
 * @ownership Returned pointer has static storage; never freed.
 * @thread_safety Read-only; safe from any thread.
 */
const cfg_desc_t *config_lookup_key(const char *key);

/**
 * @brief Look up a key descriptor by its environment variable name.
 *
 * @param[in] env Environment variable name, e.g. "API_POOL_SIZE".
 *
 * @return Pointer to the descriptor, or NULL if unknown.
 * @ownership Returned pointer has static storage; never freed.
 * @thread_safety Read-only; safe from any thread.
 */
const cfg_desc_t *config_lookup_env(const char *env);

/** @brief One allowed value of a CFG_ENUM key (from config_enums.def). */
typedef struct {
    cfg_id_t key;      /**< The enum key this value belongs to. */
    const char *value; /**< The literal value string. */
} cfg_enum_row_t;

/**
 * @brief Return the allowed values for a CFG_ENUM key.
 *
 * Values for a key are stored contiguously in config_enums.def order, so this
 * returns a pointer to the first matching row plus the count.
 *
 * @param[in]  id     Key identifier.
 * @param[out] rows   Receives a pointer to the first matching row; set to NULL
 *                    when @p id has no enum values.
 * @param[out] count  Receives the number of matching rows.
 *
 * @return 0 on success, negative errno on failure (-EINVAL for unknown id).
 * @ownership @p rows points to static storage; never freed.
 * @thread_safety Read-only; safe from any thread.
 */
int config_enum_values(cfg_id_t id, const cfg_enum_row_t **rows, size_t *count);

/**
 * @brief Write a minimal, valid config.toml to @p out.
 *
 * Every key is emitted as a commented example; secret keys use a placeholder and
 * required keys use a clearly fake value so the sample parses with
 * `tgbot --check-config` (SPEC-ADDENDUM-SSOT S3, C5). Output is deterministic.
 *
 * @param[in] out Destination stream (not closed).
 *
 * @return 0 on success, negative errno on failure.
 * @thread_safety Safe; writes only to @p out.
 */
int config_dump_example(FILE *out);

/**
 * @brief Parse a config.toml file into a flat (id, value) list.
 *
 * Implements the S3 precedence for a single file: unknown keys are errors, and
 * values are validated against their declared type. The caller supplies a
 * storage callback so the same code serves --check-config and the runtime loader.
 *
 * @param[in]  path     Path to the TOML file.
 * @param[in]  store    Callback invoked once per recognized key with its id and
 *                      raw string value (before type validation).
 * @param[in]  user     Opaque pointer passed to @p store.
 * @param[out] err_line Receives the 1-based line of the first error, or 0.
 *
 * @return 0 on success, negative errno on failure (-ENOENT, -EINVAL, -ENOMEM).
 * @thread_safety Not thread-safe (reads the file and calls @p store).
 */
int config_parse_file(const char *path, int (*store)(void *user, cfg_id_t id, const char *value),
                      void *user, size_t *err_line);

/**
 * @brief Validate a config.toml file against the schema.
 *
 * Checks that every key is known, that CFG_ENUM values are allowed, and that all
 * required keys are present. Used by `tgbot --check-config` (S3).
 *
 * @param[in]  path    Path to the TOML file.
 * @param[out] errbuf  Receives a human-readable error message (may be NULL).
 * @param[in]  errcap  Capacity of @p errbuf in bytes.
 *
 * @return 0 if the file is valid, negative errno otherwise (-ENOENT, -EINVAL).
 * @thread_safety Not thread-safe (reads the file).
 */
int config_check_file(const char *path, char *errbuf, size_t errcap);

/** @brief Maximum length of a configuration value (matches the parser's line cap). */
#define CFG_VALUE_MAX 4096

/** @brief A CLI-provided override (id, value). */
typedef struct {
    cfg_id_t id;       /**< Key to override. */
    const char *value; /**< New value. */
} cfg_override_t;

/**
 * @brief Resolved configuration values.
 *
 * Holds the effective string for every key after precedence is applied (CLI flag
 * > environment variable > config file > default). Values are owned by the
 * struct and copied out by the typed accessors.
 */
typedef struct {
    char value[CFG_COUNT][CFG_VALUE_MAX]; /**< Effective value per key. */
    int set[CFG_COUNT];                   /**< 1 if provided by env/file/CLI. */
} config_values_t;

/**
 * @brief Load configuration applying S3 precedence.
 *
 * @param[in]  path  Config file path, or NULL to skip the file.
 * @param[in]  overrides Optional per-key CLI overrides, applied last; may be NULL.
 * @param[out] out   Receives the resolved values.
 *
 * @return 0 on success, -ENOENT if @p path is given but unreadable, -EINVAL on a
 *         malformed file or an invalid value.
 * @ownership @p out is caller-owned; no heap allocation.
 * @thread_safety Not thread-safe (reads the environment and files).
 */
int config_load(const char *path, const cfg_override_t *overrides, config_values_t *out);

/**
 * @brief Return the effective string value of a key.
 *
 * @param[in] v  Loaded values.
 * @param[in] id Key.
 *
 * @return NUL-terminated value (never NULL; empty string if unset).
 * @ownership Owned by @p v.
 * @thread_safety Read-only; safe from any thread.
 */
const char *config_get_str(const config_values_t *v, cfg_id_t id);

/**
 * @brief Return the effective integer value of a key.
 *
 * @param[in] v  Loaded values.
 * @param[in] id Key.
 *
 * @return Parsed integer (0 if unset or unparsable).
 * @thread_safety Read-only; safe from any thread.
 */
long config_get_int(const config_values_t *v, cfg_id_t id);

/**
 * @brief Return the effective boolean value of a key ("true"/"1" are true).
 *
 * @param[in] v  Loaded values.
 * @param[in] id Key.
 *
 * @return 1 if true, else 0.
 * @thread_safety Read-only; safe from any thread.
 */
int config_get_bool(const config_values_t *v, cfg_id_t id);

/**
 * @brief Parse a CFG_I64_LIST value into @p out.
 *
 * Splits the comma-separated string, tolerating spaces around each entry, and
 * parses each as a signed 64-bit integer. This is the only accessor for
 * CFG_I64_LIST keys (currently `sudo_ids`).
 *
 * @param[in]  v       Loaded values.
 * @param[in]  id      Key.
 * @param[out] out     Array receiving the parsed ids.
 * @param[in]  cap     Capacity of @p out (number of entries).
 * @param[out] count   Receives the number of ids written.
 *
 * @return 0 on success (including an empty list), -EINVAL on a malformed entry
 *         or bad arguments, -ENOSPC if the list exceeds @p cap.
 * @thread_safety Read-only; safe from any thread.
 */
int config_get_i64_list(const config_values_t *v, cfg_id_t id, int64_t *out, size_t cap,
                        size_t *count);

#endif /* NM_CONFIG_H */
