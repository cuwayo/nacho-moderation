/* fuzz_update_json.c - libFuzzer harness for the update parser (A5.4).
 *
 * Feeds arbitrary bytes into the getUpdates batch parser and the single-update
 * parser; neither must crash or read out of bounds on malformed input. The
 * visitor parses every element and does a trivial read of the mapped fields so
 * the compiler cannot elide the work.
 */
#include <stddef.h>
#include <stdint.h>

#include "tg/tg_types.h"

// NOLINTNEXTLINE(misc-use-internal-linkage)
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/** @brief Visitor that touches every mapped field (see tg_updates_parse). */
static int visit(const tg_update_t *u, void *ctx)
{
    (void)ctx;
    volatile int64_t id = u->update_id + u->message.message_id + u->message.chat.id;
    volatile int kind = (int)u->kind;
    (void)id;
    (void)kind;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const char *json = (const char *)data;

    tg_update_t one;
    (void)tg_update_parse(json, size, &one);

    unsigned count = 0;
    (void)tg_updates_parse(json, size, visit, NULL, &count);
    return 0;
}
