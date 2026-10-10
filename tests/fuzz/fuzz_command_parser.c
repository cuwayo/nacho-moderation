/* fuzz_command_parser.c - libFuzzer harness for the command/duration parsers (A5.4).
 *
 * Feeds arbitrary bytes as a message text into the command/target parser and as a
 * `[duration] [reason]` tail into the duration parser. Both handle untrusted input
 * (SPEC section 5.2), so they must never read out of bounds or crash on garbage.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tg/cmd_util.h"
#include "tg/router.h"
#include "tg/util.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Bound the length so it always fits the terminator buffer. */
    char buf[512];
    size_t n = size < sizeof(buf) - 1u ? size : sizeof(buf) - 1u;
    memcpy(buf, data, n);
    buf[n] = '\0';

    int64_t seconds = 0;
    (void)util_parse_duration(buf, &seconds);

    int64_t duration = 0;
    const char *reason = NULL;
    (void)cmd_parse_duration_reason(buf, &duration, &reason);

    /* The command/target parser, both with and without target extraction. */
    cmd_invocation_t inv;
    (void)router_parse_command(buf, "MyBot", &inv);
    (void)router_parse_command_ex(buf, "MyBot", 0, &inv);
    return 0;
}
