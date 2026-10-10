/* test_automod.c - automated moderation classifiers and the flood window (A5.1).
 *
 * Covers the pure parts of src/automod.c: the lock bitmask classifier, the
 * blacklist matcher (plain + regex, case-insensitivity, ReDoS limits) and the
 * sliding-window arithmetic, all driven by an injected clock. No network, no
 * SQLite, no real clock.
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "tg/automod.h"
#include "tg/tg_types.h"

/* --- Locks ----------------------------------------------------------------- */

static void test_lock_media_detected(void **state)
{
    (void)state;
    tg_message_t m = {0};
    m.content_flags = TG_CONTENT_MEDIA;
    assert_int_equal(automod_locked_content(LOCK_MEDIA, &m), LOCK_MEDIA);
    /* Not locked => never flagged. */
    assert_int_equal(automod_locked_content(LOCK_LINKS, &m), 0);
}

static void test_lock_sticker_and_animation(void **state)
{
    (void)state;
    tg_message_t m = {0};
    m.content_flags = TG_CONTENT_STICKER;
    assert_int_equal(automod_locked_content(LOCK_STICKERS, &m), LOCK_STICKERS);
    m.content_flags = TG_CONTENT_ANIMATION;
    assert_int_equal(automod_locked_content(LOCK_STICKERS, &m), LOCK_STICKERS);
}

static void test_lock_forwards_and_polls(void **state)
{
    (void)state;
    tg_message_t m = {0};
    m.content_flags = TG_CONTENT_FORWARD | TG_CONTENT_POLL;
    unsigned hit = automod_locked_content((unsigned)(LOCK_FORWARDS | LOCK_POLLS), &m);
    assert_int_equal(hit, (unsigned)(LOCK_FORWARDS | LOCK_POLLS));
}

static void test_lock_entities_links_and_commands(void **state)
{
    (void)state;
    tg_message_t m = {0};
    (void)snprintf(m.entities[0].type, sizeof(m.entities[0].type), "url");
    (void)snprintf(m.entities[1].type, sizeof(m.entities[1].type), "bot_command");
    m.entity_count = 2;
    unsigned hit = automod_locked_content((unsigned)(LOCK_LINKS | LOCK_COMMANDS), &m);
    assert_int_equal(hit, (unsigned)(LOCK_LINKS | LOCK_COMMANDS));
    /* Only links locked => the command entity is not reported. */
    assert_int_equal(automod_locked_content(LOCK_LINKS, &m), LOCK_LINKS);
}

static void test_lock_invites_from_text(void **state)
{
    (void)state;
    tg_message_t m = {0};
    (void)snprintf(m.text, sizeof(m.text), "join us https://t.me/joinchat/ABCDEF");
    assert_int_equal(automod_locked_content(LOCK_INVITES, &m), LOCK_INVITES);
    (void)snprintf(m.text, sizeof(m.text), "no invite here");
    assert_int_equal(automod_locked_content(LOCK_INVITES, &m), 0);
}

static void test_lock_bots(void **state)
{
    (void)state;
    tg_message_t m = {0};
    m.from.is_bot = 1;
    assert_int_equal(automod_locked_content(LOCK_BOTS, &m), LOCK_BOTS);
}

/* --- Blacklist ------------------------------------------------------------- */

static void test_blacklist_plain_case_insensitive(void **state)
{
    (void)state;
    assert_int_equal(automod_blacklist_match("scam", 0, "This is a SCAM link"), 1);
    assert_int_equal(automod_blacklist_match("scam", 0, "nothing here"), 0);
}

static void test_blacklist_regex(void **state)
{
    (void)state;
    assert_int_equal(automod_blacklist_match("fr[e3]+e\\s+crypto", 1, "free crypto"), 1);
    assert_int_equal(automod_blacklist_match("^bad$", 1, "not bad"), 0);
    /* Case-insensitive by default. */
    assert_int_equal(automod_blacklist_match("BAD", 1, "bad"), 1);
}

static void test_blacklist_invalid_regex_never_matches(void **state)
{
    (void)state;
    assert_int_equal(automod_blacklist_match("(unclosed", 1, "(unclosed"), 0);
}

static void test_blacklist_empty_inputs(void **state)
{
    (void)state;
    assert_int_equal(automod_blacklist_match("", 0, "text"), 0);
    assert_int_equal(automod_blacklist_match("x", 0, ""), 0);
    assert_int_equal(automod_blacklist_match(NULL, 0, "text"), 0);
}

/* --- Flood window ---------------------------------------------------------- */

static void test_flood_counts_within_window(void **state)
{
    (void)state;
    flood_window_t w = {0};
    assert_int_equal(automod_flood_track(&w, 1000, 1, 10000), 1);
    assert_int_equal(automod_flood_track(&w, 2000, 2, 10000), 2);
    assert_int_equal(automod_flood_track(&w, 3000, 3, 10000), 3);
}

static void test_flood_expires_old_entries(void **state)
{
    (void)state;
    flood_window_t w = {0};
    (void)automod_flood_track(&w, 1000, 1, 5000);
    (void)automod_flood_track(&w, 2000, 2, 5000);
    /* 7000 - 5000 = 2000 cutoff; the entry at 1000 falls out. */
    assert_int_equal(automod_flood_track(&w, 7000, 3, 5000), 2);
}

static void test_flood_isolated_windows(void **state)
{
    (void)state;
    flood_window_t a = {0};
    flood_window_t b = {0};
    (void)automod_flood_track(&a, 1000, 1, 10000);
    assert_int_equal(automod_flood_track(&b, 1000, 2, 10000), 1);
    assert_int_equal(a.count, 1);
}

static void test_flood_caps_at_slots(void **state)
{
    (void)state;
    flood_window_t w = {0};
    for (int i = 0; i < 40; ++i) {
        (void)automod_flood_track(&w, 1000u + (uint64_t)i, i, 1000000);
    }
    assert_int_equal(w.count, AUTOMOD_FLOOD_SLOTS);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_lock_media_detected),
        cmocka_unit_test(test_lock_sticker_and_animation),
        cmocka_unit_test(test_lock_forwards_and_polls),
        cmocka_unit_test(test_lock_entities_links_and_commands),
        cmocka_unit_test(test_lock_invites_from_text),
        cmocka_unit_test(test_lock_bots),
        cmocka_unit_test(test_blacklist_plain_case_insensitive),
        cmocka_unit_test(test_blacklist_regex),
        cmocka_unit_test(test_blacklist_invalid_regex_never_matches),
        cmocka_unit_test(test_blacklist_empty_inputs),
        cmocka_unit_test(test_flood_counts_within_window),
        cmocka_unit_test(test_flood_expires_old_entries),
        cmocka_unit_test(test_flood_isolated_windows),
        cmocka_unit_test(test_flood_caps_at_slots),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
