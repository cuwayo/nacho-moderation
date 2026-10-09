/* fuzz_html_escape.c - libFuzzer harness for HTML escaping + truncation (A5.4).
 *
 * Feeds arbitrary bytes through util_html_escape into a small buffer, exercising
 * the truncation and boundary paths.
 */
#include <stddef.h>
#include <stdint.h>

#include "tg/util.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Deliberately small so truncation (-ENOSPC) is hit frequently. */
    char out[64];
    size_t outlen = 0;
    (void)util_html_escape((const char *)data, size, out, sizeof(out), &outlen);
    return 0;
}
