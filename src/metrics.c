/**
 * @file metrics.c
 * @brief Runtime counters generated from src/metrics.def (SPEC-ADDENDUM-TOOLCHAIN A8).
 *
 * @ingroup util
 */
#include "metrics.h"

#include <stddef.h>
#include <stdio.h>

/** @brief Counter storage, one slot per metrics.def row. */
static unsigned long long g_counters[METRIC_COUNT];

/* Counter names, generated from the SSOT table (metrics.def). */
/** @cond */
static const char *const g_metric_names[METRIC_COUNT] = {
#define M(id, name, brief) name,
#include "metrics.def"
#undef M
};
/** @endcond */

/**
 * @brief Report whether @p id is a valid counter index.
 *
 * @param[in] id Counter identifier.
 *
 * @return 1 if in range, else 0.
 */
static int valid(metric_id_t id)
{
    return (int)id >= 0 && (int)id < (int)METRIC_COUNT;
}

void metrics_inc(metric_id_t id)
{
    metrics_add(id, 1);
}

void metrics_add(metric_id_t id, unsigned long long delta)
{
    if (valid(id)) {
        g_counters[id] += delta;
    }
}

void metrics_set(metric_id_t id, unsigned long long value)
{
    if (valid(id)) {
        g_counters[id] = value;
    }
}

unsigned long long metrics_get(metric_id_t id)
{
    return valid(id) ? g_counters[id] : 0;
}

void metrics_dump(FILE *out)
{
    if (out == NULL) {
        return;
    }
    for (int i = 0; i < (int)METRIC_COUNT; ++i) {
        (void)fprintf(out, "%s %llu\n", g_metric_names[i], g_counters[i]);
    }
}
