/* fuzz_command_parser.c - libFuzzer harness for the duration parser (A5.4).
 *
 * Feeds arbitrary bytes as a duration literal into util_parse_duration.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tg/util.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Bound the length so it always fits the terminator buffer. */
    char buf[128];
    size_t n = size < sizeof(buf) - 1u ? size : sizeof(buf) - 1u;
    memcpy(buf, data, n);
    buf[n] = '\0';

    int64_t seconds = 0;
    (void)util_parse_duration(buf, &seconds);
    return 0;
}
