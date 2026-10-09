/**
 * @file version.h
 * @brief Build version identity for nacho-moderator.
 *
 * @ingroup util
 */
#ifndef TG_VERSION_H
#define TG_VERSION_H

/** @brief Human-readable version string. */
#define TG_VERSION_STRING "0.0.1"

/**
 * @brief Return the human-readable version string.
 *
 * @return Pointer to a string literal (never NULL).
 * @ownership Static storage duration; the caller must NOT free it.
 * @thread_safety Read-only; safe from any thread.
 */
const char *tg_version(void);

#endif /* TG_VERSION_H */
