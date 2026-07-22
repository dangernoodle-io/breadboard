#include "unity.h"
#include "bb_mdns_cache.h"

#include <string.h>
#include <stddef.h>

// Consumer struct fixture: identity-compatible leading layout followed by
// consumer-owned TXT capture fields (mirrors the pattern a real consumer,
// e.g. TaipanMiner, would define -- NOT bb_mdns_cache_entry_t itself).
typedef struct {
    char     hostname[BB_MDNS_CACHE_HOSTNAME_MAX];
    char     ip4[BB_MDNS_CACHE_IP4_MAX];
    uint16_t port;
    char     board[16];
    char     version[16];
} test_entry_t;

static const bb_mdns_txt_field_t s_fields[] = {
    { .txt_key = "board",   .dest_offset = offsetof(test_entry_t, board),   .dest_len = sizeof(((test_entry_t *)0)->board) },
    { .txt_key = "version", .dest_offset = offsetof(test_entry_t, version), .dest_len = sizeof(((test_entry_t *)0)->version) },
};

// ---------------------------------------------------------------------------
// bb_mdns_cache_apply_txt
// ---------------------------------------------------------------------------

void test_bb_mdns_cache_apply_txt_matching_keys_land(void)
{
    test_entry_t entry = {0};
    bb_mdns_txt_t txt[] = {
        { .key = (char *)"board",   .value = (char *)"tdongle-s3" },
        { .key = (char *)"version", .value = (char *)"v1.2.3" },
    };

    bb_mdns_cache_apply_txt(&entry, sizeof(entry), s_fields, 2, txt, 2);

    TEST_ASSERT_EQUAL_STRING("tdongle-s3", entry.board);
    TEST_ASSERT_EQUAL_STRING("v1.2.3", entry.version);
}

void test_bb_mdns_cache_apply_txt_non_matching_keys_skipped(void)
{
    test_entry_t entry = {0};
    bb_mdns_txt_t txt[] = {
        { .key = (char *)"unrelated", .value = (char *)"whatever" },
    };

    bb_mdns_cache_apply_txt(&entry, sizeof(entry), s_fields, 2, txt, 1);

    TEST_ASSERT_EQUAL_STRING("", entry.board);
    TEST_ASSERT_EQUAL_STRING("", entry.version);
}

void test_bb_mdns_cache_apply_txt_oversized_value_truncates(void)
{
    test_entry_t entry = {0};
    bb_mdns_txt_t txt[] = {
        { .key = (char *)"board", .value = (char *)"this-board-name-is-way-too-long-to-fit" },
    };

    bb_mdns_cache_apply_txt(&entry, sizeof(entry), s_fields, 2, txt, 1);

    TEST_ASSERT_EQUAL_INT('\0', entry.board[sizeof(entry.board) - 1]);
    TEST_ASSERT_TRUE(strlen(entry.board) < sizeof(entry.board));
    TEST_ASSERT_EQUAL_STRING_LEN("this-board-name-is-way-too-long-to-fit", entry.board,
                                  sizeof(entry.board) - 1);
}

void test_bb_mdns_cache_apply_txt_null_value_copies_empty(void)
{
    test_entry_t entry = {0};
    strcpy(entry.board, "stale");
    bb_mdns_txt_t txt[] = {
        { .key = (char *)"board", .value = NULL },
    };

    bb_mdns_cache_apply_txt(&entry, sizeof(entry), s_fields, 2, txt, 1);

    TEST_ASSERT_EQUAL_STRING("", entry.board);
}

void test_bb_mdns_cache_apply_txt_duplicate_keys_first_wins(void)
{
    test_entry_t entry = {0};
    bb_mdns_txt_t txt[] = {
        { .key = (char *)"board", .value = (char *)"first" },
        { .key = (char *)"board", .value = (char *)"second" },
    };

    bb_mdns_cache_apply_txt(&entry, sizeof(entry), s_fields, 2, txt, 2);

    TEST_ASSERT_EQUAL_STRING("first", entry.board);
}

void test_bb_mdns_cache_apply_txt_txt_entry_null_key_skipped(void)
{
    test_entry_t entry = {0};
    bb_mdns_txt_t txt[] = {
        { .key = NULL, .value = (char *)"whatever" },
        { .key = (char *)"board", .value = (char *)"tdongle-s3" },
    };

    bb_mdns_cache_apply_txt(&entry, sizeof(entry), s_fields, 2, txt, 2);

    TEST_ASSERT_EQUAL_STRING("tdongle-s3", entry.board);
}

void test_bb_mdns_cache_apply_txt_field_null_key_skipped(void)
{
    test_entry_t entry = {0};
    bb_mdns_txt_field_t fields[] = {
        { .txt_key = NULL, .dest_offset = offsetof(test_entry_t, board), .dest_len = sizeof(entry.board) },
    };
    bb_mdns_txt_t txt[] = {
        { .key = (char *)"board", .value = (char *)"tdongle-s3" },
    };

    bb_mdns_cache_apply_txt(&entry, sizeof(entry), fields, 1, txt, 1);

    TEST_ASSERT_EQUAL_STRING("", entry.board);
}

void test_bb_mdns_cache_apply_txt_field_zero_dest_len_skipped(void)
{
    test_entry_t entry = {0};
    bb_mdns_txt_field_t fields[] = {
        { .txt_key = "board", .dest_offset = offsetof(test_entry_t, board), .dest_len = 0 },
    };
    bb_mdns_txt_t txt[] = {
        { .key = (char *)"board", .value = (char *)"tdongle-s3" },
    };

    bb_mdns_cache_apply_txt(&entry, sizeof(entry), fields, 1, txt, 1);

    TEST_ASSERT_EQUAL_STRING("", entry.board);
}

void test_bb_mdns_cache_apply_txt_field_out_of_bounds_skipped(void)
{
    test_entry_t entry = {0};
    bb_mdns_txt_field_t fields[] = {
        // dest_offset + dest_len deliberately exceeds sizeof(entry).
        { .txt_key = "board", .dest_offset = sizeof(entry) - 4, .dest_len = 32 },
    };
    bb_mdns_txt_t txt[] = {
        { .key = (char *)"board", .value = (char *)"tdongle-s3" },
    };

    bb_mdns_cache_apply_txt(&entry, sizeof(entry), fields, 1, txt, 1);

    // Nothing should have been written past the struct -- field is skipped
    // entirely, so the whole entry stays zeroed.
    test_entry_t zeroed = {0};
    TEST_ASSERT_EQUAL_MEMORY(&zeroed, &entry, sizeof(entry));
}

void test_bb_mdns_cache_apply_txt_null_entry_is_noop(void)
{
    bb_mdns_txt_t txt[] = { { .key = (char *)"board", .value = (char *)"x" } };
    // Must not crash.
    bb_mdns_cache_apply_txt(NULL, sizeof(test_entry_t), s_fields, 2, txt, 1);
}

void test_bb_mdns_cache_apply_txt_zero_entry_size_is_noop(void)
{
    test_entry_t entry = {0};
    bb_mdns_txt_t txt[] = { { .key = (char *)"board", .value = (char *)"x" } };
    bb_mdns_cache_apply_txt(&entry, 0, s_fields, 2, txt, 1);
    TEST_ASSERT_EQUAL_STRING("", entry.board);
}

void test_bb_mdns_cache_apply_txt_null_fields_is_noop(void)
{
    test_entry_t entry = {0};
    bb_mdns_txt_t txt[] = { { .key = (char *)"board", .value = (char *)"x" } };
    bb_mdns_cache_apply_txt(&entry, sizeof(entry), NULL, 2, txt, 1);
    TEST_ASSERT_EQUAL_STRING("", entry.board);
}

void test_bb_mdns_cache_apply_txt_zero_field_count_is_noop(void)
{
    test_entry_t entry = {0};
    bb_mdns_txt_t txt[] = { { .key = (char *)"board", .value = (char *)"x" } };
    bb_mdns_cache_apply_txt(&entry, sizeof(entry), s_fields, 0, txt, 1);
    TEST_ASSERT_EQUAL_STRING("", entry.board);
}

void test_bb_mdns_cache_apply_txt_null_txt_is_noop(void)
{
    test_entry_t entry = {0};
    bb_mdns_cache_apply_txt(&entry, sizeof(entry), s_fields, 2, NULL, 1);
    TEST_ASSERT_EQUAL_STRING("", entry.board);
}

void test_bb_mdns_cache_apply_txt_zero_txt_count_is_noop(void)
{
    test_entry_t entry = {0};
    bb_mdns_txt_t txt[] = { { .key = (char *)"board", .value = (char *)"x" } };
    bb_mdns_cache_apply_txt(&entry, sizeof(entry), s_fields, 2, txt, 0);
    TEST_ASSERT_EQUAL_STRING("", entry.board);
}

// bb_mdns_cache_txt_serialize (the bb_json_t mirror of the walk above) and
// its tests are DELETED (B1-1149) -- see bb_mdns_cache.h's doc comment at
// the deleted declaration's former site for why. The equivalent walk is
// covered on the wire-descriptor path by test_bb_mdns_cache_wire.c
// (bb_mdns_cache_entry_wire_fill()).

// ---------------------------------------------------------------------------
// BB_MDNS_CACHE_ASSERT_IDENTITY_LAYOUT -- compile-time-only check. A build
// failure here (not a runtime assertion) means test_entry_t's identity
// layout has drifted from bb_mdns_cache_entry_t.
// ---------------------------------------------------------------------------

BB_MDNS_CACHE_ASSERT_IDENTITY_LAYOUT(test_entry_t);
