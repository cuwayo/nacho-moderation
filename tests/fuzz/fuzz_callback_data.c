/* fuzz_callback_data.c - libFuzzer harness for callback-data parsing (A5.4).
 *
 * Feeds arbitrary bytes as inline-keyboard callback_data into the settings-panel
 * parser. Callback data is untrusted input (SPEC section 10): the parser must
 * never read out of bounds or crash on garbage, and must reject anything that is
 * not a well-formed `set:<chat>:<user>:<key>` payload.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tg/cmd_settings.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Telegram caps callback_data at 64 bytes; bound to that plus a NUL. */
    char buf[65];
    size_t n = size < sizeof(buf) - 1u ? size : sizeof(buf) - 1u;
    memcpy(buf, data, n);
    buf[n] = '\0';

    int64_t chat = 0;
    int64_t user = 0;
    const char *key = NULL;
    (void)cmd_settings_parse_callback(buf, &chat, &user, &key);
    return 0;
}
