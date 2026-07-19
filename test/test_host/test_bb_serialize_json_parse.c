// Host tests for bb_serialize_json_parse_bytes() -- the composed
// bb_serialize_parse_fn adapter registered under BB_FORMAT_JSON (B1-1030).
// See its doc comment in bb_serialize_json.h for the scratch-layout/
// lifetime contract this exercises.

#include "unity.h"
#include "bb_serialize.h"
#include "bb_serialize_json.h"

#include <string.h>

// ---------------------------------------------------------------------------
// A descriptor with a scalar top-level field plus a nested object, matching
// the flavor of doc bb_serialize_populate() already round-trips elsewhere --
// exercising this adapter's own composition (scan -> tok recorder ->
// populate_from_tok), not populate's own field-type coverage.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t id;
    bool    armed;
} pos_snap_t;

typedef struct {
    char       label[16];
    pos_snap_t pos;
} nested_snap_t;

static const bb_serialize_field_t s_pos_fields[] = {
    { .key = "id", .type = BB_TYPE_I64, .offset = offsetof(pos_snap_t, id) },
    { .key = "armed", .type = BB_TYPE_BOOL, .offset = offsetof(pos_snap_t, armed) },
};

static const bb_serialize_field_t s_nested_fields[] = {
    { .key = "label", .type = BB_TYPE_STR, .offset = offsetof(nested_snap_t, label), .max_len = sizeof(((nested_snap_t *)0)->label) },
    { .key = "pos", .type = BB_TYPE_OBJ, .offset = offsetof(nested_snap_t, pos),
      .children = s_pos_fields, .n_children = 2 },
};

static const bb_serialize_desc_t s_nested_desc = {
    .type_name = "nested_snap_t", .fields = s_nested_fields, .n_fields = 2, .snap_size = sizeof(nested_snap_t),
};

// Comfortably fits bb_mem_arena's own header + the recorder + the populate
// ctx + the default-capacity token pool (BB_SERIALIZE_JSON_TOK_POOL_DEFAULT_CAP
// * sizeof(bb_serialize_json_tok_t) alone is 48*48 = 2304 bytes), plus
// headroom for the escape-decode arena.
#define SCRATCH_CAP 4096
static char s_scratch[SCRATCH_CAP];

void test_bb_serialize_json_parse_bytes_null_args_return_invalid_arg(void)
{
    bb_serialize_populate_t src;
    const char *doc = "{}";

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_serialize_json_parse_bytes(NULL, 2, s_scratch, SCRATCH_CAP, &src));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_serialize_json_parse_bytes(doc, 2, NULL, SCRATCH_CAP, &src));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG,
                       bb_serialize_json_parse_bytes(doc, 2, s_scratch, SCRATCH_CAP, NULL));
}

void test_bb_serialize_json_parse_bytes_flat_and_nested_roundtrip(void)
{
    const char *doc = "{\"label\":\"unit-1\",\"pos\":{\"id\":7,\"armed\":true}}";

    bb_serialize_populate_t src;
    bb_err_t rc = bb_serialize_json_parse_bytes(doc, strlen(doc), s_scratch, SCRATCH_CAP, &src);
    TEST_ASSERT_EQUAL(BB_OK, rc);

    nested_snap_t dst = { 0 };
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_nested_desc, &dst, &src));

    TEST_ASSERT_EQUAL_STRING("unit-1", dst.label);
    TEST_ASSERT_EQUAL_INT64(7, dst.pos.id);
    TEST_ASSERT_TRUE(dst.pos.armed);
}

// A string value that needs the recorder's escape-decode arena (not a
// zero-copy slice of `doc`) -- proves the leftover-bytes-become-arena
// carving actually works end to end, not just the zero-copy path.
void test_bb_serialize_json_parse_bytes_arena_backed_escaped_string(void)
{
    const char *doc = "{\"label\":\"ab\\ncd\",\"pos\":{\"id\":1,\"armed\":false}}";

    bb_serialize_populate_t src;
    bb_err_t rc = bb_serialize_json_parse_bytes(doc, strlen(doc), s_scratch, SCRATCH_CAP, &src);
    TEST_ASSERT_EQUAL(BB_OK, rc);

    nested_snap_t dst = { 0 };
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_nested_desc, &dst, &src));

    TEST_ASSERT_EQUAL_MEMORY("ab\ncd", dst.label, 5);
    TEST_ASSERT_EQUAL_INT64(1, dst.pos.id);
    TEST_ASSERT_FALSE(dst.pos.armed);
}

// Malformed JSON: the scan itself fails and the error propagates verbatim;
// `*out_src` is left unbound (never inspected on a non-BB_OK return).
void test_bb_serialize_json_parse_bytes_malformed_json_propagates_scan_error(void)
{
    const char *doc = "{\"label\":}";  // missing value

    bb_serialize_populate_t src;
    bb_err_t rc = bb_serialize_json_parse_bytes(doc, strlen(doc), s_scratch, SCRATCH_CAP, &src);
    TEST_ASSERT_NOT_EQUAL(BB_OK, rc);
}

// Undersized scratch -- too small to lay out even the fixed-size control
// structs (recorder + populate ctx) plus the default-capacity token pool --
// fails closed with BB_ERR_NO_SPACE rather than a partial/garbage bind.
void test_bb_serialize_json_parse_bytes_undersized_scratch_returns_no_space(void)
{
    char tiny_scratch[8];
    bb_serialize_populate_t src;
    const char *doc = "{}";

    bb_err_t rc = bb_serialize_json_parse_bytes(doc, strlen(doc), tiny_scratch, sizeof(tiny_scratch), &src);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

// Zero-length scratch -- bb_mem_arena_init() rejects this too (same guard,
// same BB_ERR_NO_SPACE outcome as the tiny-but-nonzero case above); pinned
// as its own edge case since a caller passing a literal 0 is a distinct,
// plausible misuse from an under-budgeted-but-nonzero buffer.
void test_bb_serialize_json_parse_bytes_zero_scratch_cap_returns_no_space(void)
{
    bb_serialize_populate_t src;
    const char *doc = "{}";

    bb_err_t rc = bb_serialize_json_parse_bytes(doc, strlen(doc), s_scratch, 0, &src);
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, rc);
}

// Enough scratch for the control structs + token pool, but nothing left
// over for the escape-decode arena -- a document with no escaped strings
// still parses fine (the arena legitimately ends up 0 bytes and is never
// needed). The exact byte count that leaves 0 bytes free depends on
// bb_mem_arena's own (opaque) header size and alignment padding, neither of
// which this test should hardcode -- instead, probe for the SMALLEST
// `scratch_cap` this call ever succeeds at. Growing the buffer by 1 byte at
// a time grows the tail (arena) region by exactly 1 byte too (every fixed
// allocation before it starts at the same offset regardless of total
// buffer size), so the first successful size in a byte-granular sweep is,
// by construction, the size at which the escape-decode arena is exactly 0
// bytes -- deterministically exercising that branch without needing to
// know bb_mem_arena's internal layout.
static char s_probe_scratch[4096];

void test_bb_serialize_json_parse_bytes_no_arena_headroom_escape_free_doc_still_parses(void)
{
    const char *doc = "{\"label\":\"unit-1\",\"pos\":{\"id\":3,\"armed\":true}}";

    size_t min_ok = 0;
    for (size_t sz = 1; sz <= sizeof(s_probe_scratch); sz++) {
        bb_serialize_populate_t probe_src;
        if (bb_serialize_json_parse_bytes(doc, strlen(doc), s_probe_scratch, sz, &probe_src) == BB_OK) {
            min_ok = sz;
            break;
        }
    }
    TEST_ASSERT_NOT_EQUAL(0, min_ok);

    bb_serialize_populate_t src;
    TEST_ASSERT_EQUAL(BB_OK,
                       bb_serialize_json_parse_bytes(doc, strlen(doc), s_probe_scratch, min_ok, &src));

    nested_snap_t dst = { 0 };
    TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_nested_desc, &dst, &src));
    TEST_ASSERT_EQUAL_STRING("unit-1", dst.label);
    TEST_ASSERT_EQUAL_INT64(3, dst.pos.id);
}

// Regression (review B1-1030 HIGH): a `scratch_cap` that isn't
// alignment-clean must not spuriously fail the WHOLE parse. SCRATCH_CAP
// itself (4096) is a multiple of _Alignof(max_align_t), so every fixed-size
// allocation before the escape-decode arena carve lands on an aligned
// offset too, leaving an aligned remainder -- +1/+3 deliberately break
// that. Pre-fix, the adapter requested EXACTLY bb_mem_arena_free_bytes()
// for the escape-decode arena; bb_mem_arena_alloc() rounds that request UP
// before checking it against the true remaining bytes, so a non-aligned
// remainder made the request round PAST what's left and the arena carve
// (and therefore the entire parse) failed with BB_ERR_NO_SPACE -- even
// though the escape-decode arena only needed a handful of bytes. Uses a
// document containing an escaped string so the escape-decode arena is
// actually exercised (not just carved and left unused).
static char s_misaligned_scratch[SCRATCH_CAP + 8];

void test_bb_serialize_json_parse_bytes_misaligned_scratch_cap_still_parses_escaped_string(void)
{
    const char *doc = "{\"label\":\"ab\\ncd\",\"pos\":{\"id\":9,\"armed\":true}}";
    const size_t misaligned_caps[] = { SCRATCH_CAP + 1, SCRATCH_CAP + 3 };

    for (size_t i = 0; i < sizeof(misaligned_caps) / sizeof(misaligned_caps[0]); i++) {
        bb_serialize_populate_t src;
        bb_err_t rc = bb_serialize_json_parse_bytes(doc, strlen(doc), s_misaligned_scratch,
                                                      misaligned_caps[i], &src);
        TEST_ASSERT_EQUAL(BB_OK, rc);

        nested_snap_t dst = { 0 };
        TEST_ASSERT_EQUAL(BB_OK, bb_serialize_populate(&s_nested_desc, &dst, &src));
        TEST_ASSERT_EQUAL_MEMORY("ab\ncd", dst.label, 5);
        TEST_ASSERT_EQUAL_INT64(9, dst.pos.id);
        TEST_ASSERT_TRUE(dst.pos.armed);
    }
}
