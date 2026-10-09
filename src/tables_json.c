/* tables_json.c - deterministic JSON views of the SSOT tables.
 *
 * SPEC-ADDENDUM-SSOT S5. Every table is rendered from its .def file via the same
 * X-macro mechanism used elsewhere, so the JSON can never drift from the tables.
 * Output ordering is table order and contains no timestamps, which makes the dumps
 * byte-identical across runs and across GCC/Clang builds (S9.4).
 *
 * @file tables_json.c
 * @brief Introspection dumps for commands, config, and the S4 tables.
 * @ingroup util
 */
#include "tables_json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "commands.h"
#include "config.h"
#include "tables.h"

/* --- String spellings of the table tokens ---------------------------------- */

static const char *const role_names[] = {"ROLE_ANYONE", "ROLE_ADMIN", "ROLE_OWNER", "ROLE_SUDO"};

static const char *const right_names[] = {"RIGHT_NONE", "RIGHT_RESTRICT", "RIGHT_DELETE",
                                          "RIGHT_PIN",  "RIGHT_PROMOTE",  "RIGHT_CHANGE_INFO"};

static const char *const cfg_type_names[] = {"CFG_STR",      "CFG_INT",      "CFG_BOOL",
                                             "CFG_DURATION", "CFG_I64_LIST", "CFG_ENUM"};

static const char *const flag_names[] = {"CMDF_TARGET", "CMDF_REPLY_REQUIRED", "CMDF_DURATION",
                                         "CMDF_REASON", "CMDF_GROUP_ONLY",     "CMDF_HIDDEN"};

static const unsigned flag_bits[] = {CMDF_TARGET, CMDF_REPLY_REQUIRED, CMDF_DURATION,
                                     CMDF_REASON, CMDF_GROUP_ONLY,     CMDF_HIDDEN};

/* Map a table token value to its name; idx is long so enum values widen without
 * a signedness change (the warning flags reject the implicit int conversion). */
static const char *name_of(const char *const *names, size_t n, long idx)
{
    if (idx < 0 || (size_t)idx >= n) {
        return "UNKNOWN";
    }
    return names[idx];
}

/* --- Serialization helpers ------------------------------------------------- */

static int write_doc(FILE *out, yyjson_mut_doc *doc)
{
    size_t len = 0;
    char *json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &len);
    if (json == NULL) {
        return -ENOMEM;
    }
    int rc = 0;
    if (fwrite(json, 1, len, out) != len) {
        rc = -EIO;
    } else {
        (void)fputc('\n', out);
    }
    free(json);
    return rc;
}

/* --- Table builders (populate a caller-provided array) --------------------- */

static int build_commands(yyjson_mut_doc *doc, yyjson_mut_val *arr)
{
    for (size_t i = 0; i < (size_t)CMD_COUNT; ++i) {
        const cmd_desc_t *c = &cmd_table[i];
        yyjson_mut_val *o = yyjson_mut_obj(doc);
        yyjson_mut_val *flags = yyjson_mut_arr(doc);
        if (o == NULL || flags == NULL) {
            return -ENOMEM;
        }
        for (size_t f = 0; f < sizeof(flag_bits) / sizeof(flag_bits[0]); ++f) {
            if ((c->flags & flag_bits[f]) != 0u &&
                !yyjson_mut_arr_add_str(doc, flags, flag_names[f])) {
                return -ENOMEM;
            }
        }
        if (!yyjson_mut_arr_add_val(arr, o) || !yyjson_mut_obj_add_str(doc, o, "id", c->name) ||
            !yyjson_mut_obj_add_str(doc, o, "min_role", name_of(role_names, 4, c->min_role)) ||
            !yyjson_mut_obj_add_str(doc, o, "user_right", name_of(right_names, 6, c->user_right)) ||
            !yyjson_mut_obj_add_str(doc, o, "bot_right", name_of(right_names, 6, c->bot_right)) ||
            !yyjson_mut_obj_add_val(doc, o, "flags", flags) ||
            !yyjson_mut_obj_add_str(doc, o, "usage", c->usage) ||
            !yyjson_mut_obj_add_str(doc, o, "brief", c->brief) ||
            !yyjson_mut_obj_add_bool(doc, o, "hidden", (c->flags & CMDF_HIDDEN) != 0u)) {
            return -ENOMEM;
        }
    }
    return 0;
}

/* Append every allowed value of an enum key to @p values. Non-enum keys and keys
 * with no values are left as an empty array. */
static int fill_enum_values(yyjson_mut_doc *doc, yyjson_mut_val *values, cfg_id_t id)
{
    const cfg_enum_row_t *rows = NULL;
    size_t count = 0;
    if (config_enum_values(id, &rows, &count) != 0) {
        return 0;
    }
    for (size_t e = 0; e < count; ++e) {
        if (!yyjson_mut_arr_add_str(doc, values, rows[e].value)) {
            return -ENOMEM;
        }
    }
    return 0;
}

static int build_config(yyjson_mut_doc *doc, yyjson_mut_val *arr)
{
    for (size_t i = 0; i < (size_t)CFG_COUNT; ++i) {
        const cfg_desc_t *k = &config_table[i];
        yyjson_mut_val *o = yyjson_mut_obj(doc);
        yyjson_mut_val *values = yyjson_mut_arr(doc);
        if (o == NULL || values == NULL || fill_enum_values(doc, values, k->id) != 0) {
            return -ENOMEM;
        }
        if (!yyjson_mut_arr_add_val(arr, o) || !yyjson_mut_obj_add_str(doc, o, "id", k->env) ||
            !yyjson_mut_obj_add_str(doc, o, "key", k->key) ||
            !yyjson_mut_obj_add_str(doc, o, "env", k->env) ||
            !yyjson_mut_obj_add_str(doc, o, "type", name_of(cfg_type_names, 6, k->type)) ||
            !yyjson_mut_obj_add_str(doc, o, "default", k->def) ||
            !yyjson_mut_obj_add_bool(doc, o, "restart", k->restart != 0) ||
            !yyjson_mut_obj_add_bool(doc, o, "secret", k->secret != 0) ||
            !yyjson_mut_obj_add_bool(doc, o, "required", config_is_required(k->id) != 0) ||
            !yyjson_mut_obj_add_val(doc, o, "enum_values", values) ||
            !yyjson_mut_obj_add_str(doc, o, "brief", k->brief)) {
            return -ENOMEM;
        }
    }
    return 0;
}

static int build_locks(yyjson_mut_doc *doc, yyjson_mut_val *arr)
{
    static const struct {
        const char *id;
        const char *name;
        const char *brief;
    } rows[] = {
#define L(id, name, bit, brief) {#id, name, brief},
#include "locks.def"
#undef L
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        yyjson_mut_val *o = yyjson_mut_obj(doc);
        if (o == NULL || !yyjson_mut_arr_add_val(arr, o) ||
            !yyjson_mut_obj_add_str(doc, o, "id", rows[i].id) ||
            !yyjson_mut_obj_add_str(doc, o, "name", rows[i].name) ||
            !yyjson_mut_obj_add_str(doc, o, "brief", rows[i].brief)) {
            return -ENOMEM;
        }
    }
    return 0;
}

static int build_api_methods(yyjson_mut_doc *doc, yyjson_mut_val *arr)
{
    static const struct {
        const char *id;
        const char *method;
        const char *bucket;
        int idempotent;
        const char *brief;
    } rows[] = {
#define M(id, method, bucket, idem, brief) {#id, method, #bucket, idem, brief},
#include "api_methods.def"
#undef M
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        yyjson_mut_val *o = yyjson_mut_obj(doc);
        if (o == NULL || !yyjson_mut_arr_add_val(arr, o) ||
            !yyjson_mut_obj_add_str(doc, o, "id", rows[i].id) ||
            !yyjson_mut_obj_add_str(doc, o, "method", rows[i].method) ||
            !yyjson_mut_obj_add_str(doc, o, "bucket", rows[i].bucket) ||
            !yyjson_mut_obj_add_bool(doc, o, "idempotent", rows[i].idempotent != 0) ||
            !yyjson_mut_obj_add_str(doc, o, "brief", rows[i].brief)) {
            return -ENOMEM;
        }
    }
    return 0;
}

static int build_errors(yyjson_mut_doc *doc, yyjson_mut_val *arr)
{
    static const struct {
        const char *id;
        long code;
        const char *level;
        int retry;
        const char *brief;
    } rows[] = {
#define E(id, code, level, retry, brief) {#id, code, #level, retry, brief},
#include "errors.def"
#undef E
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        yyjson_mut_val *o = yyjson_mut_obj(doc);
        if (o == NULL || !yyjson_mut_arr_add_val(arr, o) ||
            !yyjson_mut_obj_add_str(doc, o, "id", rows[i].id) ||
            !yyjson_mut_obj_add_int(doc, o, "code", rows[i].code) ||
            !yyjson_mut_obj_add_str(doc, o, "log_level", rows[i].level) ||
            !yyjson_mut_obj_add_bool(doc, o, "retry", rows[i].retry != 0) ||
            !yyjson_mut_obj_add_str(doc, o, "brief", rows[i].brief)) {
            return -ENOMEM;
        }
    }
    return 0;
}

static int build_metrics(yyjson_mut_doc *doc, yyjson_mut_val *arr)
{
    static const struct {
        const char *id;
        const char *name;
        const char *brief;
    } rows[] = {
#define M(id, name, brief) {#id, name, brief},
#include "metrics.def"
#undef M
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        yyjson_mut_val *o = yyjson_mut_obj(doc);
        if (o == NULL || !yyjson_mut_arr_add_val(arr, o) ||
            !yyjson_mut_obj_add_str(doc, o, "id", rows[i].id) ||
            !yyjson_mut_obj_add_str(doc, o, "name", rows[i].name) ||
            !yyjson_mut_obj_add_str(doc, o, "brief", rows[i].brief)) {
            return -ENOMEM;
        }
    }
    return 0;
}

/* --- Public entry points --------------------------------------------------- */

static int dump_one(FILE *out, int (*build)(yyjson_mut_doc *, yyjson_mut_val *))
{
    if (out == NULL) {
        return -EINVAL;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc != NULL ? yyjson_mut_arr(doc) : NULL;
    if (doc == NULL || root == NULL) {
        yyjson_mut_doc_free(doc);
        return -ENOMEM;
    }
    yyjson_mut_doc_set_root(doc, root);
    int rc = build(doc, root);
    if (rc == 0) {
        rc = write_doc(out, doc);
    }
    yyjson_mut_doc_free(doc);
    return rc;
}

int tables_dump_commands_json(FILE *out)
{
    return dump_one(out, build_commands);
}

int tables_dump_config_json(FILE *out)
{
    return dump_one(out, build_config);
}

int tables_dump_all_json(FILE *out)
{
    if (out == NULL) {
        return -EINVAL;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc != NULL ? yyjson_mut_obj(doc) : NULL;
    if (doc == NULL || root == NULL) {
        yyjson_mut_doc_free(doc);
        return -ENOMEM;
    }
    yyjson_mut_doc_set_root(doc, root);

    static const struct {
        const char *key;
        int (*build)(yyjson_mut_doc *, yyjson_mut_val *);
    } sections[] = {
        {"commands", build_commands},       {"config", build_config}, {"locks", build_locks},
        {"api_methods", build_api_methods}, {"errors", build_errors}, {"metrics", build_metrics},
    };

    for (size_t i = 0; i < sizeof(sections) / sizeof(sections[0]); ++i) {
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        if (arr == NULL || !yyjson_mut_obj_add_val(doc, root, sections[i].key, arr) ||
            sections[i].build(doc, arr) != 0) {
            yyjson_mut_doc_free(doc);
            return -ENOMEM;
        }
    }

    int rc = write_doc(out, doc);
    yyjson_mut_doc_free(doc);
    return rc;
}
