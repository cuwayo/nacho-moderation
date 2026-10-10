/* api_methods.c - API method registry derived from src/api_methods.def.
 *
 * SPEC-ADDENDUM-SSOT S4. api_method_table is generated from the X-macro table so
 * the enum, the descriptor table and the lookup can never drift apart.
 *
 * @file api_methods.c
 * @brief API method descriptor table and lookup.
 * @ingroup api
 */
#include "api_methods.h"

#include <string.h>

/* Table generated from the single source of truth (src/api_methods.def). */
const api_method_t api_method_table[METHOD_COUNT] = {
#define M(id, method, bucket, idem, brief) {METHOD_##id, method, bucket, idem, brief},
#include "api_methods.def"
#undef M
};

const api_method_t *api_method_lookup(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < (size_t)METHOD_COUNT; ++i) {
        if (strcmp(api_method_table[i].name, name) == 0) {
            return &api_method_table[i];
        }
    }
    return NULL;
}

int api_method_is_idempotent(method_id_t id)
{
    if ((int)id < 0 || (int)id >= (int)METHOD_COUNT) {
        return 0;
    }
    return api_method_table[id].idempotent;
}

const char *api_method_name(method_id_t id)
{
    if ((int)id < 0 || (int)id >= (int)METHOD_COUNT) {
        return "";
    }
    return api_method_table[id].name;
}
