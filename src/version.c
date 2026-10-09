/**
 * @file version.c
 * @brief Build version identity.
 *
 * Part of the M0 toolchain skeleton: the smallest non-trivial module used to
 * prove the build, sanitizer, test, and coverage wiring end-to-end.
 *
 * @ingroup util
 */
#include "tg/version.h"

const char *tg_version(void)
{
    return TG_VERSION_STRING;
}
