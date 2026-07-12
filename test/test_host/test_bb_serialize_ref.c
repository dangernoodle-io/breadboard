// BB_TYPE_REF composed-section reference-child -- host-only, synthetic
// descriptors + a stub resolver (no bb_cache dependency; a production
// resolver backed by bb_cache_serialize is deferred, see B1-786's scope
// note). Reuses the recorded-emit mock harness from test_bb_serialize.c.

#include "unity.h"
#include "bb_serialize.h"

#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Recording emit mock -- identical shape to test_bb_serialize.c's harness
// (kept file-local: Unity host tests build each .c as an independent TU, so
// sharing static file-scope state across TUs isn't possible without a
// header; this mirrors the existing pattern rather than forking behavior).
// ---------------------------------------------------------------------------

typedef enum {
    OP_BEGIN_OBJ,
    OP_END_OBJ,
    OP_BEGIN_ARR,
    OP_END_ARR,
    OP_I64,
    OP_U64,
    OP_F64,
    OP_BOOL,
    OP_STR,
    OP_NUL,
} rec_op_t;

typedef struct {
    rec_op_t    op;
    const char *key;
    union {
        int64_t     i64;
        uint64_t    u64;
        double      f64;
        bool        b;
    } num;
    char   str_val[64];
    size_t str_len;
} rec_t;

#define REC_MAX 64

static rec_t  s_rec[REC_MAX];
static size_t s_rec_n;

static void rec_reset(void) { s_rec_n = 0; }

static rec_t *rec_push(rec_op_t op, const char *key)
{
    TEST_ASSERT_TRUE(s_rec_n < REC_MAX);
    rec_t *r = &s_rec[s_rec_n++];
    memset(r, 0, sizeof(*r));
    r->op = op;
    r->key = key;
    return r;
}

static void mock_begin_obj(void *ctx, const char *key) { (void)ctx; rec_push(OP_BEGIN_OBJ, key); }
static void mock_end_obj(void *ctx) { (void)ctx; rec_push(OP_END_OBJ, NULL); }
static void mock_begin_arr(void *ctx, const char *key) { (void)ctx; rec_push(OP_BEGIN_ARR, key); }
static void mock_end_arr(void *ctx) { (void)ctx; rec_push(OP_END_ARR, NULL); }

static void mock_emit_i64(void *ctx, const char *key, int64_t v)
{
    (void)ctx;
    rec_push(OP_I64, key)->num.i64 = v;
}

static void mock_emit_u64(void *ctx, const char *key, uint64_t v)
{
    (void)ctx;
    rec_push(OP_U64, key)->num.u64 = v;
}

static void mock_emit_f64(void *ctx, const char *key, double v)
{
    (void)ctx;
    rec_push(OP_F64, key)->num.f64 = v;
}

static void mock_emit_bool(void *ctx, const char *key, bool v)
{
    (void)ctx;
    rec_push(OP_BOOL, key)->num.b = v;
}

static void mock_emit_str(void *ctx, const char *key, const char *s, size_t len)
{
    (void)ctx;
    rec_t *r = rec_push(OP_STR, key);
    size_t n = len < sizeof(r->str_val) - 1 ? len : sizeof(r->str_val) - 1;
    if (s && n) memcpy(r->str_val, s, n);
    r->str_val[n] = '\0';
    r->str_len = len;
}

static void mock_emit_null(void *ctx, const char *key) { (void)ctx; rec_push(OP_NUL, key); }

static const bb_serialize_emit_t s_mock_emit = {
    .format_id = BB_FORMAT_NONE,
    .ctx = NULL,
    .begin_obj = mock_begin_obj,
    .end_obj = mock_end_obj,
    .begin_arr = mock_begin_arr,
    .end_arr = mock_end_arr,
    .emit_i64 = mock_emit_i64,
    .emit_u64 = mock_emit_u64,
    .emit_f64 = mock_emit_f64,
    .emit_bool = mock_emit_bool,
    .emit_str = mock_emit_str,
    .emit_null = mock_emit_null,
};

static void assert_key(const char *expected, const char *actual)
{
    if (!expected) {
        TEST_ASSERT_NULL(actual);
    } else {
        TEST_ASSERT_NOT_NULL(actual);
        TEST_ASSERT_EQUAL_STRING(expected, actual);
    }
}

// ---------------------------------------------------------------------------
// Fixtures: a "wifi" sibling section referenced from a "system" parent.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t rssi;
} wifi_snap_t;

static const bb_serialize_field_t s_wifi_fields[] = {
    { .key = "rssi", .type = BB_TYPE_I64, .offset = offsetof(wifi_snap_t, rssi) },
};

static const bb_serialize_desc_t s_wifi_desc = {
    .type_name = "wifi_snap_t",
    .fields = s_wifi_fields,
    .n_fields = 1,
    .snap_size = sizeof(wifi_snap_t),
};

static wifi_snap_t s_wifi_snap = { .rssi = -55 };

typedef struct {
    int64_t uptime;
} system_snap_t;

static const bb_serialize_field_t s_system_fields[] = {
    { .key = "uptime", .type = BB_TYPE_I64, .offset = offsetof(system_snap_t, uptime) },
    { .key = "wifi", .type = BB_TYPE_REF, .ref_key = "net.wifi" },
};

static const bb_serialize_desc_t s_system_desc = {
    .type_name = "system_snap_t",
    .fields = s_system_fields,
    .n_fields = 2,
    .snap_size = sizeof(system_snap_t),
};

// A descriptor identical to s_system_desc but WITHOUT the REF field, used to
// prove the resolve==NULL / resolver-false paths are byte-identical to
// simply never having declared the field.
static const bb_serialize_field_t s_system_no_ref_fields[] = {
    { .key = "uptime", .type = BB_TYPE_I64, .offset = offsetof(system_snap_t, uptime) },
};

static const bb_serialize_desc_t s_system_no_ref_desc = {
    .type_name = "system_snap_t",
    .fields = s_system_no_ref_fields,
    .n_fields = 1,
    .snap_size = sizeof(system_snap_t),
};

// ---------------------------------------------------------------------------
// Stub resolver -- a static table mapping ref_key -> {desc,snap}. Returns
// false for any key not in the table (the "unregistered sibling" path).
// ---------------------------------------------------------------------------

typedef struct {
    const char                *ref_key;
    const bb_serialize_desc_t *desc;
    const void                *snap;
} stub_ref_entry_t;

static const stub_ref_entry_t s_stub_table[] = {
    { .ref_key = "net.wifi", .desc = &s_wifi_desc, .snap = &s_wifi_snap },
};

static bool stub_resolve(const char *ref_key, void *ctx, bb_serialize_ref_t *out)
{
    (void)ctx;
    for (size_t i = 0; i < sizeof(s_stub_table) / sizeof(s_stub_table[0]); i++) {
        if (strcmp(s_stub_table[i].ref_key, ref_key) == 0) {
            out->desc = s_stub_table[i].desc;
            out->snap = s_stub_table[i].snap;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 1. happy path: REF resolves, sibling fields render inline at the correct
// wire key/position.
// ---------------------------------------------------------------------------

void test_bb_serialize_ref_happy_path_resolves_inline(void)
{
    rec_reset();
    system_snap_t snap = { .uptime = 100 };

    bb_serialize_walk_ref(&s_system_desc, &snap, &s_mock_emit, stub_resolve, NULL);

    TEST_ASSERT_EQUAL_UINT(4, s_rec_n);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[0].op);
    assert_key("uptime", s_rec[0].key);
    TEST_ASSERT_EQUAL_INT64(100, s_rec[0].num.i64);
    TEST_ASSERT_EQUAL(OP_BEGIN_OBJ, s_rec[1].op);
    assert_key("wifi", s_rec[1].key);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[2].op);
    assert_key("rssi", s_rec[2].key);
    TEST_ASSERT_EQUAL_INT64(-55, s_rec[2].num.i64);
    TEST_ASSERT_EQUAL(OP_END_OBJ, s_rec[3].op);
}

// ---------------------------------------------------------------------------
// 2. null-sibling: resolver returns false -> key fully omitted, byte-
// identical to a desc without the field.
// ---------------------------------------------------------------------------

static bool stub_resolve_always_false(const char *ref_key, void *ctx, bb_serialize_ref_t *out)
{
    (void)ref_key;
    (void)ctx;
    (void)out;
    return false;
}

void test_bb_serialize_ref_unregistered_sibling_omits_field(void)
{
    rec_reset();
    system_snap_t snap = { .uptime = 100 };

    bb_serialize_walk_ref(&s_system_desc, &snap, &s_mock_emit, stub_resolve_always_false, NULL);

    TEST_ASSERT_EQUAL_UINT(1, s_rec_n);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[0].op);
    assert_key("uptime", s_rec[0].key);

    // Byte-identical (in recorded-op terms) to a descriptor that never
    // declared the REF field at all.
    rec_reset();
    bb_serialize_walk(&s_system_no_ref_desc, &snap, &s_mock_emit);
    TEST_ASSERT_EQUAL_UINT(1, s_rec_n);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[0].op);
    assert_key("uptime", s_rec[0].key);
}

// ---------------------------------------------------------------------------
// 3. resolve == NULL: plain bb_serialize_walk() on a desc containing a REF
// field -> same omission (proves the wrapper is behavior-preserving).
// ---------------------------------------------------------------------------

void test_bb_serialize_ref_plain_walk_no_resolver_omits_field(void)
{
    rec_reset();
    system_snap_t snap = { .uptime = 100 };

    bb_serialize_walk(&s_system_desc, &snap, &s_mock_emit);

    TEST_ASSERT_EQUAL_UINT(1, s_rec_n);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[0].op);
    assert_key("uptime", s_rec[0].key);
}

// ---------------------------------------------------------------------------
// 3b. a `present`-false REF field short-circuits before resolution is ever
// attempted -- even with a resolver that would otherwise succeed.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t uptime;
    bool    wifi_present;
} gated_system_snap_t;

static bool gate_wifi_present(const void *snap)
{
    return ((const gated_system_snap_t *)snap)->wifi_present;
}

static const bb_serialize_field_t s_gated_system_fields[] = {
    { .key = "uptime", .type = BB_TYPE_I64, .offset = offsetof(gated_system_snap_t, uptime) },
    { .key = "wifi", .type = BB_TYPE_REF, .ref_key = "net.wifi", .present = gate_wifi_present },
};

static const bb_serialize_desc_t s_gated_system_desc = {
    .type_name = "gated_system_snap_t",
    .fields = s_gated_system_fields,
    .n_fields = 2,
    .snap_size = sizeof(gated_system_snap_t),
};

void test_bb_serialize_ref_present_false_short_circuits_before_resolve(void)
{
    rec_reset();
    gated_system_snap_t snap = { .uptime = 7, .wifi_present = false };

    bb_serialize_walk_ref(&s_gated_system_desc, &snap, &s_mock_emit, stub_resolve, NULL);

    TEST_ASSERT_EQUAL_UINT(1, s_rec_n);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[0].op);
    assert_key("uptime", s_rec[0].key);
}

void test_bb_serialize_ref_present_true_resolves(void)
{
    rec_reset();
    gated_system_snap_t snap = { .uptime = 7, .wifi_present = true };

    bb_serialize_walk_ref(&s_gated_system_desc, &snap, &s_mock_emit, stub_resolve, NULL);

    TEST_ASSERT_EQUAL_UINT(4, s_rec_n);
    TEST_ASSERT_EQUAL(OP_BEGIN_OBJ, s_rec[1].op);
    assert_key("wifi", s_rec[1].key);
}

// ---------------------------------------------------------------------------
// 3c. resolver returns true but leaves desc/snap unset (buggy resolver /
// future cache-race) -> field omitted, byte-identical to the unregistered
// case, no NULL-deref crash.
// ---------------------------------------------------------------------------

static bool stub_resolve_true_null_desc(const char *ref_key, void *ctx, bb_serialize_ref_t *out)
{
    (void)ref_key;
    (void)ctx;
    out->desc = NULL;
    out->snap = &s_wifi_snap;
    return true;
}

void test_bb_serialize_ref_resolver_true_null_desc_omits_field(void)
{
    rec_reset();
    system_snap_t snap = { .uptime = 100 };

    bb_serialize_walk_ref(&s_system_desc, &snap, &s_mock_emit, stub_resolve_true_null_desc, NULL);

    TEST_ASSERT_EQUAL_UINT(1, s_rec_n);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[0].op);
    assert_key("uptime", s_rec[0].key);
}

static bool stub_resolve_true_null_snap(const char *ref_key, void *ctx, bb_serialize_ref_t *out)
{
    (void)ref_key;
    (void)ctx;
    out->desc = &s_wifi_desc;
    out->snap = NULL;
    return true;
}

void test_bb_serialize_ref_resolver_true_null_snap_omits_field(void)
{
    rec_reset();
    system_snap_t snap = { .uptime = 100 };

    bb_serialize_walk_ref(&s_system_desc, &snap, &s_mock_emit, stub_resolve_true_null_snap, NULL);

    TEST_ASSERT_EQUAL_UINT(1, s_rec_n);
    TEST_ASSERT_EQUAL(OP_I64, s_rec[0].op);
    assert_key("uptime", s_rec[0].key);
}

// ---------------------------------------------------------------------------
// 4. cycle: 2-node A<->B REF cycle -> exact truncation at depth 8,
// deterministic bytes.
// ---------------------------------------------------------------------------

typedef struct {
    int64_t marker;
} cycle_a_snap_t;

typedef struct {
    int64_t marker;
} cycle_b_snap_t;

static const bb_serialize_field_t s_cycle_a_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(cycle_a_snap_t, marker) },
    { .key = "b", .type = BB_TYPE_REF, .ref_key = "cycle.b" },
};

static const bb_serialize_desc_t s_cycle_a_desc = {
    .type_name = "cycle_a_snap_t",
    .fields = s_cycle_a_fields,
    .n_fields = 2,
    .snap_size = sizeof(cycle_a_snap_t),
};

static const bb_serialize_field_t s_cycle_b_fields[] = {
    { .key = "marker", .type = BB_TYPE_I64, .offset = offsetof(cycle_b_snap_t, marker) },
    { .key = "a", .type = BB_TYPE_REF, .ref_key = "cycle.a" },
};

static const bb_serialize_desc_t s_cycle_b_desc = {
    .type_name = "cycle_b_snap_t",
    .fields = s_cycle_b_fields,
    .n_fields = 2,
    .snap_size = sizeof(cycle_b_snap_t),
};

static cycle_a_snap_t s_cycle_a_snap = { .marker = 1 };
static cycle_b_snap_t s_cycle_b_snap = { .marker = 2 };

static bool cycle_resolve(const char *ref_key, void *ctx, bb_serialize_ref_t *out)
{
    (void)ctx;
    if (strcmp(ref_key, "cycle.b") == 0) {
        out->desc = &s_cycle_b_desc;
        out->snap = &s_cycle_b_snap;
        return true;
    }
    if (strcmp(ref_key, "cycle.a") == 0) {
        out->desc = &s_cycle_a_desc;
        out->snap = &s_cycle_a_snap;
        return true;
    }
    return false;
}

void test_bb_serialize_ref_cycle_truncates_at_max_depth(void)
{
    rec_reset();

    bb_serialize_walk_ref(&s_cycle_a_desc, &s_cycle_a_snap, &s_mock_emit, cycle_resolve, NULL);

    // Depth 0..MAX_DEPTH-1 each alternately emit {marker, begin_obj(ref)}
    // (2 records) and recurse; at depth == MAX_DEPTH the REF guard bails
    // before begin_obj/resolve, emitting only the final marker (1 record,
    // no ref hop). Unwinding then emits one end_obj per recursed level.
    // Total: MAX_DEPTH*2 (marker+begin_obj pairs) + 1 (final marker) +
    // MAX_DEPTH (end_obj unwinds) -- identical shape to the plain-OBJ
    // depth-guard cycle test in test_bb_serialize.c.
    TEST_ASSERT_EQUAL_UINT(BB_SERIALIZE_MAX_DEPTH * 2 + 1 + BB_SERIALIZE_MAX_DEPTH, s_rec_n);

    // Exact byte-level sequence: alternating markers 1,2,1,2,... and
    // "b","a","b","a",... ref keys, down to the depth cap.
    for (unsigned lvl = 0; lvl < BB_SERIALIZE_MAX_DEPTH; lvl++) {
        const rec_t *marker_rec = &s_rec[lvl * 2];
        const rec_t *ref_rec = &s_rec[lvl * 2 + 1];
        int64_t expected_marker = (lvl % 2 == 0) ? 1 : 2;
        const char *expected_ref_key = (lvl % 2 == 0) ? "b" : "a";

        TEST_ASSERT_EQUAL(OP_I64, marker_rec->op);
        assert_key("marker", marker_rec->key);
        TEST_ASSERT_EQUAL_INT64(expected_marker, marker_rec->num.i64);

        TEST_ASSERT_EQUAL(OP_BEGIN_OBJ, ref_rec->op);
        assert_key(expected_ref_key, ref_rec->key);
    }

    // The final (depth-capped) record is a lone marker, no begin_obj.
    const rec_t *final_rec = &s_rec[BB_SERIALIZE_MAX_DEPTH * 2];
    TEST_ASSERT_EQUAL(OP_I64, final_rec->op);
    assert_key("marker", final_rec->key);
    int64_t expected_final_marker = (BB_SERIALIZE_MAX_DEPTH % 2 == 0) ? 1 : 2;
    TEST_ASSERT_EQUAL_INT64(expected_final_marker, final_rec->num.i64);

    // Unwind: BB_SERIALIZE_MAX_DEPTH end_obj records, one per recursed level.
    for (unsigned i = 0; i < BB_SERIALIZE_MAX_DEPTH; i++) {
        const rec_t *end_rec = &s_rec[BB_SERIALIZE_MAX_DEPTH * 2 + 1 + i];
        TEST_ASSERT_EQUAL(OP_END_OBJ, end_rec->op);
    }
}
