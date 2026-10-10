/**
 * @file metrics.h
 * @brief Runtime counters generated from src/metrics.def (SPEC-ADDENDUM-TOOLCHAIN A8).
 *
 * A fixed array of monotonically increasing counters, one per metrics.def row.
 * The loop thread is the only writer; SIGUSR1 dumps them to the log. Counter names
 * and help strings come from the SSOT table, so the dump and the docs cannot drift.
 *
 * @ingroup util
 */
#ifndef NM_METRICS_H
#define NM_METRICS_H

#include <stdio.h>

/** @brief Counter identifiers, one per metrics.def row, plus METRIC_COUNT. */
typedef enum {
/** @cond */
#define M(id, name, brief) METRIC_##id,
#include "metrics.def"
#undef M
    /** @endcond */
    METRIC_COUNT /**< Number of registered counters. */
} metric_id_t;

/**
 * @brief Increment a counter by one.
 *
 * @param[in] id Counter identifier (out-of-range ids are ignored).
 * @thread_safety Loop thread only.
 */
void metrics_inc(metric_id_t id);

/**
 * @brief Add @p delta to a counter.
 *
 * @param[in] id    Counter identifier (out-of-range ids are ignored).
 * @param[in] delta Amount to add.
 * @thread_safety Loop thread only.
 */
void metrics_add(metric_id_t id, unsigned long long delta);

/**
 * @brief Set a gauge-style counter to an absolute value.
 *
 * @param[in] id    Counter identifier (out-of-range ids are ignored).
 * @param[in] value New value.
 * @thread_safety Loop thread only.
 */
void metrics_set(metric_id_t id, unsigned long long value);

/**
 * @brief Read a counter's current value.
 *
 * @param[in] id Counter identifier.
 *
 * @return The value, or 0 for an out-of-range id.
 * @thread_safety Read-only.
 */
unsigned long long metrics_get(metric_id_t id);

/**
 * @brief Write every counter as `name value` lines, one per metric.
 *
 * @param[in] out Destination stream (not closed).
 * @thread_safety Loop thread only.
 */
void metrics_dump(FILE *out);

#endif /* NM_METRICS_H */
