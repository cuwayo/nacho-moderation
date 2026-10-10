/**
 * @file errors.h
 * @brief Error-class codes, generated from src/errors.def (SPEC-ADDENDUM-SSOT S4).
 *
 * The numeric codes are defined once in the table; this header turns them into
 * ERR_* constants. They are negative errno-style values that do not collide with
 * the standard errno range, so callers can distinguish bot errors from libc ones.
 *
 * @ingroup util
 */
#ifndef TG_ERRORS_H
#define TG_ERRORS_H

/** @brief Error classes, one per errors.def row. */
typedef enum {
/** @cond */
#define E(id, code, level, retry, brief) ERR_##id = (code),
#include "errors.def"
#undef E
    /** @endcond */
} err_class_t;

#endif /* TG_ERRORS_H */
