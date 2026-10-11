/* config.c - configuration schema, required set, and enum values.
 *
 * SPEC-ADDENDUM-SSOT S3. config_table is generated from src/config.def; the enum
 * value sets come from src/config_enums.def. Nothing here hard-codes a key name.
 *
 * @file config.c
 * @brief Configuration descriptor table, required-key set, and enum value lookup.
 * @ingroup util
 */
#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tables.h"

/** @brief One conditional requirement row (from CFG_REQUIRED_IF in config.def). */
typedef struct {
    cfg_id_t id;       /**< Key that becomes required. */
    cfg_id_t dep;      /**< Key whose value selects the requirement. */
    const char *value; /**< Required value of @ref dep. */
    size_t min_len;    /**< Minimum length of @ref id's value (0 = no floor). */
} cfg_req_if_t;

/* Schema generated from the single source of truth (src/config.def).
 * Positional initializers match cfg_desc_t's field order (key, env, def, brief,
 * id, type, restart, secret); designated initializers cannot be used because the
 * K() parameter names would collide with the field designators. */
const cfg_desc_t config_table[CFG_COUNT] = {
#define K(id, key, env, type, def, restart, secret, brief)                                         \
    {key, env, def, brief, CFG_##id, type, restart, secret},
#define CFG_REQUIRED(id)
#define CFG_REQUIRED_IF(id, dep, val, minlen)
#include "config.def"
#undef CFG_REQUIRED_IF
#undef CFG_REQUIRED
#undef K
};

/* Required-key set generated from the CFG_REQUIRED(...) list in config.def.
 * Unlisted keys stay zero-initialized, so only designated entries are needed. */
static const int config_required[CFG_COUNT] = {
#define K(id, key, env, type, def, restart, secret, brief)
#define CFG_REQUIRED(id) [CFG_##id] = 1,
#define CFG_REQUIRED_IF(id, dep, val, minlen)
#include "config.def"
#undef CFG_REQUIRED_IF
#undef CFG_REQUIRED
#undef K
};

/* Conditional requirements generated from CFG_REQUIRED_IF(...) in config.def. */
static const cfg_req_if_t config_required_if[] = {
#define K(id, key, env, type, def, restart, secret, brief)
#define CFG_REQUIRED(id)
#define CFG_REQUIRED_IF(id, dep, val, minlen) {CFG_##id, CFG_##dep, val, minlen},
#include "config.def"
#undef CFG_REQUIRED_IF
#undef CFG_REQUIRED
#undef K
};

/* Flat (key, value) list generated from src/config_enums.def. */
static const cfg_enum_row_t config_enums[] = {
#define E(key, value) {CFG_##key, value},
#include "config_enums.def"
#undef E
};

#define CONFIG_ENUM_COUNT (sizeof(config_enums) / sizeof(config_enums[0]))

int config_is_required(cfg_id_t id)
{
    if ((int)id < 0 || (int)id >= (int)CFG_COUNT) {
        return 0;
    }
    return config_required[id];
}

/* Check the CFG_REQUIRED_IF rules: a key that must be present (and long enough)
 * when another key has a particular value. Returns 0 or -EINVAL. */
static int validate_required_if(const config_values_t *v, char *errbuf, size_t errcap)
{
    for (size_t r = 0; r < sizeof(config_required_if) / sizeof(config_required_if[0]); ++r) {
        const cfg_req_if_t *req = &config_required_if[r];
        if (strcmp(v->value[req->dep], req->value) != 0) {
            continue;
        }
        const char *val = v->value[req->id];
        if (val[0] == '\0') {
            if (errbuf != NULL) {
                (void)snprintf(errbuf, errcap, "key \"%s\" is required when \"%s\" is \"%s\"",
                               config_table[req->id].key, config_table[req->dep].key, req->value);
            }
            return -EINVAL;
        }
        if (strlen(val) < req->min_len) {
            if (errbuf != NULL) {
                (void)snprintf(errbuf, errcap,
                               "key \"%s\" must be at least %zu characters when \"%s\" is \"%s\"",
                               config_table[req->id].key, req->min_len, config_table[req->dep].key,
                               req->value);
            }
            return -EINVAL;
        }
    }
    return 0;
}

int config_validate(const config_values_t *v, char *errbuf, size_t errcap)
{
    if (v == NULL) {
        return -EINVAL;
    }
    for (size_t i = 0; i < (size_t)CFG_COUNT; ++i) {
        if (config_required[i] && v->value[i][0] == '\0') {
            if (errbuf != NULL) {
                (void)snprintf(errbuf, errcap, "missing required key \"%s\"", config_table[i].key);
            }
            return -EINVAL;
        }
    }
    return validate_required_if(v, errbuf, errcap);
}

const cfg_desc_t *config_lookup_key(const char *key)
{
    if (key == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < (size_t)CFG_COUNT; ++i) {
        if (strcmp(config_table[i].key, key) == 0) {
            return &config_table[i];
        }
    }
    return NULL;
}

const cfg_desc_t *config_lookup_env(const char *env)
{
    if (env == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < (size_t)CFG_COUNT; ++i) {
        if (strcmp(config_table[i].env, env) == 0) {
            return &config_table[i];
        }
    }
    return NULL;
}

int config_enum_values(cfg_id_t id, const cfg_enum_row_t **rows, size_t *count)
{
    if ((int)id < 0 || (int)id >= (int)CFG_COUNT || rows == NULL || count == NULL) {
        return -EINVAL;
    }

    size_t first = CONFIG_ENUM_COUNT;
    size_t n = 0;
    for (size_t i = 0; i < CONFIG_ENUM_COUNT; ++i) {
        if (config_enums[i].key == id) {
            if (first == CONFIG_ENUM_COUNT) {
                first = i;
            }
            ++n;
        }
    }

    if (n == 0) {
        *rows = NULL;
        *count = 0;
        return 0;
    }
    *rows = &config_enums[first];
    *count = n;
    return 0;
}

/* Render one value as it appears in the sample TOML. Secrets and required keys
 * get obviously fake placeholders so the sample still parses (C5). */
static void render_example_value(const cfg_desc_t *d, char *buf, size_t cap)
{
    if (d->secret && config_is_required(d->id)) {
        (void)snprintf(buf, cap, "\"123456:REPLACE_ME\"");
        return;
    }
    if (d->secret) {
        (void)snprintf(buf, cap, "\"REPLACE_ME\"");
        return;
    }

    const char *def = d->def;
    switch (d->type) {
    case CFG_INT:
        (void)snprintf(buf, cap, "%s", def[0] != '\0' ? def : "0");
        break;
    case CFG_BOOL:
        (void)snprintf(buf, cap, "%s", def[0] != '\0' ? def : "false");
        break;
    default:
        (void)snprintf(buf, cap, "\"%s\"", def);
        break;
    }
}

/* Return the section of a dotted key ("webhook.listen" -> "webhook"; "mode" -> ""). */
static const char *key_section(const char *key, char *buf, size_t cap)
{
    const char *dot = strchr(key, '.');
    if (dot == NULL) {
        return "";
    }
    size_t n = (size_t)(dot - key);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(buf, key, n);
    buf[n] = '\0';
    return buf;
}

/* Return the leaf of a dotted key ("webhook.listen" -> "listen"; "mode" -> "mode"). */
static const char *key_leaf(const char *key)
{
    const char *dot = strchr(key, '.');
    return dot == NULL ? key : dot + 1;
}

static void emit_key(FILE *out, const cfg_desc_t *d, const char *leaf)
{
    char value[256];
    render_example_value(d, value, sizeof(value));
    (void)fprintf(out, "# %s\n", d->brief);
    (void)fprintf(out, "# env: %s\n", d->env);
    (void)fprintf(out, "%s = %s\n\n", leaf, value);
}

/* Return 1 if @p section is the first key section to appear at or after index
 * @p before in the table (used to emit each section once). */
static int section_first_seen(const char *section, size_t before)
{
    for (size_t j = 0; j < before; ++j) {
        char prev[128];
        if (strcmp(key_section(config_table[j].key, prev, sizeof(prev)), section) == 0) {
            return 0;
        }
    }
    return 1;
}

static void emit_section(FILE *out, const char *section)
{
    (void)fprintf(out, "[%s]\n\n", section);
    for (size_t j = 0; j < (size_t)CFG_COUNT; ++j) {
        char cur[128];
        if (strcmp(key_section(config_table[j].key, cur, sizeof(cur)), section) == 0) {
            emit_key(out, &config_table[j], key_leaf(config_table[j].key));
        }
    }
}

int config_dump_example(FILE *out)
{
    if (out == NULL) {
        return -EINVAL;
    }

    (void)fprintf(out, "# nacho-moderator configuration sample\n"
                       "# Generated by `tgbot --dump-config-example`; do not edit by hand.\n"
                       "# Copy to config.toml and edit. Precedence: CLI flag > environment\n"
                       "# variable > this file > built-in default.\n"
                       "\n");

    /* Top-level keys first (TOML bare keys must precede any [table] header). */
    for (size_t i = 0; i < (size_t)CFG_COUNT; ++i) {
        if (strchr(config_table[i].key, '.') == NULL) {
            emit_key(out, &config_table[i], config_table[i].key);
        }
    }

    /* Then each section, once, in order of first appearance in the table. */
    for (size_t i = 0; i < (size_t)CFG_COUNT; ++i) {
        char section[128];
        if (key_section(config_table[i].key, section, sizeof(section))[0] == '\0') {
            continue;
        }
        if (section_first_seen(section, i)) {
            emit_section(out, section);
        }
    }
    return 0;
}

/* Strip surrounding double quotes from a TOML string value, in place. */
static void unquote(char *s)
{
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

/* Advance past leading spaces/tabs and strip trailing whitespace in place.
 * Returns the pointer to the first significant character. */
static char *trim(char *p)
{
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    size_t len = strlen(p);
    while (len > 0 &&
           (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) {
        p[--len] = '\0';
    }
    return p;
}

/* Parse a "[section]" header line into @p out. Returns 0 or -EINVAL. */
static int parse_section(char *p, char *out, size_t cap)
{
    char *close = strchr(p, ']');
    if (close == NULL) {
        return -EINVAL;
    }
    *close = '\0';
    (void)snprintf(out, cap, "%s", p + 1);
    return 0;
}

/* Split a "key = value" line into @p key and @p val (both within @p p).
 * Returns 0 or -EINVAL when there is no '='. */
static int split_kv(char *p, char **key, char **val)
{
    char *eq = strchr(p, '=');
    if (eq == NULL) {
        return -EINVAL;
    }
    *eq = '\0';
    *key = trim(p);
    *val = trim(eq + 1);
    unquote(*val);
    return 0;
}

/* Build the fully qualified key ("section.key" or "key") into @p out. */
static void build_full_key(const char *section, const char *key, char *out, size_t cap)
{
    if (section[0] != '\0') {
        (void)snprintf(out, cap, "%s.%s", section, key);
    } else {
        (void)snprintf(out, cap, "%s", key);
    }
}

/* Handle one non-blank line: update @p section, or look up and store a key.
 * Returns 0 on success (including comments/blank lines), negative errno on error. */
static int process_line(char *p, char *section, size_t secap,
                        int (*store)(void *, cfg_id_t, const char *), void *user)
{
    if (*p == '\0' || *p == '#') {
        return 0;
    }
    if (*p == '[') {
        return parse_section(p, section, secap);
    }

    char *key = NULL;
    char *val = NULL;
    int rc = split_kv(p, &key, &val);
    if (rc != 0) {
        return rc;
    }
    char full[4352];
    build_full_key(section, key, full, sizeof(full));

    const cfg_desc_t *d = config_lookup_key(full);
    if (d == NULL) {
        return -EINVAL;
    }
    return store(user, d->id, val);
}

int config_parse_file(const char *path, int (*store)(void *user, cfg_id_t id, const char *value),
                      void *user, size_t *err_line)
{
    if (path == NULL || store == NULL) {
        return -EINVAL;
    }
    if (err_line != NULL) {
        *err_line = 0;
    }

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -errno;
    }

    char section[128] = "";
    char line[4096];
    size_t lineno = 0;
    int rc = 0;

    while (rc == 0 && fgets(line, sizeof(line), f) != NULL) {
        ++lineno;
        rc = process_line(trim(line), section, sizeof(section), store, user);
    }

    (void)fclose(f);
    if (rc != 0 && err_line != NULL) {
        *err_line = lineno;
    }
    return rc;
}

/** @brief Validation state threaded through config_parse_file's store callback. */
typedef struct {
    int seen[CFG_COUNT];       /**< 1 once a key has been provided. */
    char *errbuf;              /**< Error message buffer (may be NULL). */
    size_t errcap;             /**< Capacity of errbuf. */
    int failed;                /**< 1 once a validation error has been recorded. */
    config_values_t *resolved; /**< Values captured for conditional validation (may be NULL). */
} check_state_t;

static int check_store(void *user, cfg_id_t id, const char *value)
{
    check_state_t *st = user;
    st->seen[id] = 1;
    if (st->resolved != NULL) {
        (void)snprintf(st->resolved->value[id], CFG_VALUE_MAX, "%s", value);
        st->resolved->set[id] = 1;
    }

    const cfg_desc_t *d = &config_table[id];
    if (d->type != CFG_ENUM) {
        return 0;
    }

    const cfg_enum_row_t *rows = NULL;
    size_t count = 0;
    if (config_enum_values(id, &rows, &count) != 0) {
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(rows[i].value, value) == 0) {
            return 0;
        }
    }
    if (st->errbuf != NULL) {
        (void)snprintf(st->errbuf, st->errcap, "invalid value \"%s\" for key \"%s\"", value,
                       d->key);
    }
    st->failed = 1;
    return -EINVAL;
}

int config_check_file(const char *path, char *errbuf, size_t errcap)
{
    config_values_t resolved;
    memset(&resolved, 0, sizeof(resolved));
    for (size_t i = 0; i < (size_t)CFG_COUNT; ++i) {
        (void)snprintf(resolved.value[i], CFG_VALUE_MAX, "%s", config_table[i].def);
    }

    check_state_t st = {.errbuf = errbuf, .errcap = errcap, .failed = 0, .resolved = &resolved};
    size_t err_line = 0;

    int rc = config_parse_file(path, check_store, &st, &err_line);
    if (rc != 0) {
        if (errbuf != NULL && !st.failed) {
            (void)snprintf(errbuf, errcap, "error at line %zu in %s", err_line, path);
        }
        return rc;
    }

    for (size_t i = 0; i < (size_t)CFG_COUNT; ++i) {
        if (config_is_required((cfg_id_t)i) && !st.seen[i]) {
            if (errbuf != NULL) {
                (void)snprintf(errbuf, errcap, "missing required key \"%s\"", config_table[i].key);
            }
            return -EINVAL;
        }
    }

    /* Conditional requirements (e.g. the webhook secret) depend on values that may
     * be defaults or come from the file, so validate the resolved set built from
     * defaults + this file. Environment variables and CLI flags are separate
     * sources, validated by the runtime loader, not by --check-config. */
    return config_validate(&resolved, errbuf, errcap);
}

/* --- Loader (S3 precedence: CLI > env > file > default) -------------------- */

/* Validate @p value against the key's type; returns 0 or -EINVAL. */
static int validate_value(cfg_id_t id, const char *value)
{
    const cfg_desc_t *d = &config_table[id];
    if (d->type == CFG_ENUM) {
        const cfg_enum_row_t *rows = NULL;
        size_t count = 0;
        if (config_enum_values(id, &rows, &count) != 0) {
            return -EINVAL;
        }
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(rows[i].value, value) == 0) {
                return 0;
            }
        }
        return -EINVAL;
    }
    return 0;
}

/* Store one value into the resolved set. */
static void store_value(config_values_t *out, cfg_id_t id, const char *value)
{
    (void)snprintf(out->value[id], CFG_VALUE_MAX, "%s", value);
    out->set[id] = 1;
}

static int load_store(void *user, cfg_id_t id, const char *value)
{
    config_values_t *out = user;
    store_value(out, id, value);
    return 0;
}

int config_load(const char *path, const cfg_override_t *overrides, config_values_t *out)
{
    if (out == NULL) {
        return -EINVAL;
    }
    memset(out, 0, sizeof(*out));

    /* 1. Defaults. */
    for (size_t i = 0; i < (size_t)CFG_COUNT; ++i) {
        store_value(out, (cfg_id_t)i, config_table[i].def);
    }

    /* 2. Config file (if provided). */
    if (path != NULL) {
        size_t err_line = 0;
        int rc = config_parse_file(path, load_store, out, &err_line);
        if (rc != 0) {
            return rc;
        }
    }

    /* 3. Environment variables. */
    for (size_t i = 0; i < (size_t)CFG_COUNT; ++i) {
        const char *env = getenv(config_table[i].env);
        if (env != NULL) {
            store_value(out, (cfg_id_t)i, env);
        }
    }

    /* 4. CLI overrides (highest precedence). */
    if (overrides != NULL) {
        for (size_t i = 0; overrides[i].value != NULL; ++i) {
            store_value(out, overrides[i].id, overrides[i].value);
        }
    }

    /* Validate enum values from every source. Required-key and conditional
     * requirements are enforced by config_validate(), which --check-config and
     * the runtime startup path both call. */
    for (size_t i = 0; i < (size_t)CFG_COUNT; ++i) {
        if (out->value[i][0] != '\0' && validate_value((cfg_id_t)i, out->value[i]) != 0) {
            return -EINVAL;
        }
    }
    return 0;
}

const char *config_get_str(const config_values_t *v, cfg_id_t id)
{
    if (v == NULL || (int)id < 0 || (int)id >= (int)CFG_COUNT) {
        return "";
    }
    return v->value[id];
}

long config_get_int(const config_values_t *v, cfg_id_t id)
{
    const char *s = config_get_str(v, id);
    if (s[0] == '\0') {
        return 0;
    }
    errno = 0;
    char *end = NULL;
    long val = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return 0;
    }
    return val;
}

int config_get_bool(const config_values_t *v, cfg_id_t id)
{
    const char *s = config_get_str(v, id);
    return strcmp(s, "true") == 0 || strcmp(s, "1") == 0 || strcmp(s, "yes") == 0;
}

/** @brief Skip spaces, tabs and commas in a list value. @return Advanced pointer. */
static const char *skip_list_separators(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == ',') {
        s++;
    }
    return s;
}

int config_get_i64_list(const config_values_t *v, cfg_id_t id, int64_t *out, size_t cap,
                        size_t *count)
{
    if (out == NULL || count == NULL) {
        return -EINVAL;
    }
    *count = 0;

    const char *s = skip_list_separators(config_get_str(v, id));
    while (*s != '\0') {
        errno = 0;
        char *end = NULL;
        long long val = strtoll(s, &end, 10);
        if (end == s || errno != 0) {
            return -EINVAL;
        }
        if (*count >= cap) {
            return -ENOSPC;
        }
        out[(*count)++] = (int64_t)val;

        /* Only spaces/tabs may precede a separator (or the end of the list). */
        s = end;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (*s != '\0' && *s != ',') {
            return -EINVAL;
        }
        s = skip_list_separators(s);
    }
    return 0;
}
