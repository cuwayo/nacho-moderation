/* fuzz_api_response.c - libFuzzer harness for the Bot API response parser (A5.4).
 *
 * Feeds arbitrary bytes into tg_api_parse_result and the getUpdates offset
 * extractor; neither must crash or read out of bounds on malformed input.
 */
#include <stddef.h>
#include <stdint.h>

#include "tg/api_pool.h"
#include "tg/polling.h"
#include "tg/tg_api.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const char *json = (const char *)data;
    api_result_t res = {0};
    (void)tg_api_parse_result(json, size, &res);

    int64_t offset = 0;
    unsigned count = 0;
    (void)poller_advance_offset(&offset, json, size, &count);
    return 0;
}
