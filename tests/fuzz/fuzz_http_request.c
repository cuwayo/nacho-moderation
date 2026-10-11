/**
 * @file fuzz_http_request.c
 * @brief libFuzzer harness for the inbound HTTP request parser (A5.4).
 *
 * Feeds arbitrary bytes (including one byte at a time) into the webhook request
 * parser and asserts it never crashes and always terminates in a defined state.
 *
 * @ingroup http
 */
#include <stddef.h>
#include <stdint.h>

#include "tg/http.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    http_req_t req;
    if (http_req_init(&req, 0) != 0) {
        return 0;
    }

    /* Feed the whole buffer, then re-parse one byte at a time in a second parser
     * so chunk-boundary handling is exercised too. */
    size_t consumed = 0;
    (void)http_req_feed(&req, (const char *)data, size, &consumed);
    http_req_free(&req);

    http_req_t slow;
    if (http_req_init(&slow, 0) == 0) {
        for (size_t i = 0; i < size; ++i) {
            size_t c = 0;
            if (http_req_feed(&slow, (const char *)data + i, 1, &c) != 0) {
                break;
            }
        }
        http_req_free(&slow);
    }
    return 0;
}
