/* fuzz_moderation_args.c - libFuzzer harness for the moderation arg parser (A5.4).
 *
 * Feeds arbitrary bytes as the `[duration] [reason]` tail into
 * cmd_parse_duration_reason. The parser is untrusted-input handling (SPEC section
 * 5.2), so it must never read out of bounds or crash on garbage.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tg/cmd_util.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char buf[512];
    size_t n = size < sizeof(buf) - 1u ? size : sizeof(buf) - 1u;
    memcpy(buf, data, n);
    buf[n] = '\0';

    int64_t duration = 0;
    const char *reason = NULL;
    (void)cmd_parse_duration_reason(buf, &duration, &reason);
    return 0;
}
