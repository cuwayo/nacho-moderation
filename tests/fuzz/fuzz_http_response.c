/* fuzz_http_response.c - libFuzzer harness for the HTTP response parser (A5.4).
 *
 * Feeds arbitrary bytes (in random-sized slices) into http_resp_feed and checks
 * the parser never crashes or reads out of bounds.
 */
#include <stddef.h>
#include <stdint.h>

#include "tg/http.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    http_resp_t r;
    if (http_resp_init(&r, 64UL * 1024UL) != 0) {
        return 0;
    }

    size_t off = 0;
    while (off < size) {
        /* Vary the slice length from the input so split points are explored. */
        size_t chunk = (size_t)data[off] + 1u;
        if (chunk > size - off) {
            chunk = size - off;
        }
        size_t consumed = 0;
        int rc = http_resp_feed(&r, (const char *)data + off, chunk, &consumed);
        if (rc != 0) {
            break;
        }
        off += consumed == 0 ? chunk : consumed;
    }

    http_resp_free(&r);
    return 0;
}
