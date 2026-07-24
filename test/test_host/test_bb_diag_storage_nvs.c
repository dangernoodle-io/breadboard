// Tests for bb_diag_storage_nvs -- exercises bb_diag_storage_nvs_iter()
// (the exact production code path, no mirror) against a fake "nvs"
// bb_storage backend (mirrors test_bb_storage.c's fake-backend idiom) and
// the REAL bb_settings_nv_overlay_entries() (host-portable schema source).
// BB_ARR_STREAM conversion (no fixed row-count cap): B1-1077 PR-2 -- every
// test drives the section's real two-phase (COUNT then FILL) contract, the
// same sequence the ESP-IDF dispatcher (platform/espidf/bb_diag/
// bb_diag_section_dispatch.c) drives around one request.
#include "unity.h"
#include "bb_diag_storage_nvs.h"
#include "bb_diag_storage_nvs_test.h"
#include "bb_mem_test.h"
#include "bb_storage.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Fake "nvs" backend -- a mutable fixture (s_fixture/s_fixture_count) so
 * each test can supply its own entry list without re-registering a new
 * backend name (the fill fn hardcodes backend "nvs").
 * ---------------------------------------------------------------------------*/
static const bb_storage_entry_t *s_fixture;
static size_t                    s_fixture_count;
static size_t                    s_list_entries_call_count;
static size_t                    s_list_entries_fail_on_call;  // 0 = never fail

static bb_err_t stub_get(void *impl, const bb_storage_addr_t *addr, void *buf, size_t cap, size_t *out_len)
{
    (void)impl; (void)addr; (void)buf; (void)cap;
    if (out_len) *out_len = 0;
    return BB_ERR_NOT_FOUND;
}
static bb_err_t stub_set(void *impl, const bb_storage_addr_t *addr, const void *buf, size_t len)
{
    (void)impl; (void)addr; (void)buf; (void)len;
    return BB_OK;
}
static bb_err_t stub_erase(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl; (void)addr;
    return BB_OK;
}
static bool stub_exists(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl; (void)addr;
    return false;
}

static bb_err_t fake_nvs_list_entries(void *impl, const char *ns_or_dir, bb_storage_entry_t *out,
                                       size_t cap, size_t *count)
{
    (void)impl; (void)ns_or_dir;
    s_list_entries_call_count++;
    if (s_list_entries_fail_on_call != 0 && s_list_entries_call_count == s_list_entries_fail_on_call) {
        return BB_ERR_INVALID_STATE;
    }
    *count = s_fixture_count;
    size_t n = s_fixture_count < cap ? s_fixture_count : cap;
    for (size_t i = 0; i < n; i++) {
        if (out) out[i] = s_fixture[i];
    }
    return BB_OK;
}

static bb_err_t fake_nvs_get_stats(void *impl, bb_storage_stats_t *out)
{
    (void)impl;
    out->used_bytes      = 100;
    out->free_bytes      = 900;
    out->total_bytes     = 1000;
    out->namespace_count = 4;
    return BB_OK;
}

static bb_err_t fake_nvs_get_stats_fails(void *impl, bb_storage_stats_t *out)
{
    (void)impl; (void)out;
    return BB_ERR_INVALID_STATE;
}

static void reset_all(void)
{
    bb_storage_test_reset();
    s_fixture = NULL;
    s_fixture_count = 0;
    s_list_entries_call_count = 0;
    s_list_entries_fail_on_call = 0;
}

static void register_fake_nvs(void)
{
    bb_storage_vtable_t vt = {
        .get           = stub_get,
        .set           = stub_set,
        .erase         = stub_erase,
        .exists        = stub_exists,
        .list_entries  = fake_nvs_list_entries,
        .get_stats     = fake_nvs_get_stats,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_register_backend("nvs", &vt, NULL));
}

static void register_fake_nvs_no_enum(void)
{
    bb_storage_vtable_t vt = {
        .get = stub_get, .set = stub_set, .erase = stub_erase, .exists = stub_exists,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_register_backend("nvs", &vt, NULL));
}

static void register_fake_nvs_stats_fails(void)
{
    bb_storage_vtable_t vt = {
        .get           = stub_get,
        .set           = stub_set,
        .erase         = stub_erase,
        .exists        = stub_exists,
        .list_entries  = fake_nvs_list_entries,
        .get_stats     = fake_nvs_get_stats_fails,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_register_backend("nvs", &vt, NULL));
}

static void set_fixture(const bb_storage_entry_t *entries, size_t count)
{
    s_fixture = entries;
    s_fixture_count = count;
}

/* ---------------------------------------------------------------------------
 * Two-phase test helper -- drives the SAME COUNT-then-FILL sequence the
 * ESP-IDF dispatcher drives, against a caller-owned row buffer standing in
 * for the dispatcher's arena. Returns the phase-2 result; `*out_rows` and
 * `*out_row_count` give the caller the composed rows to assert against.
 * ---------------------------------------------------------------------------*/
#define TEST_ROW_CAP 64
static bb_diag_storage_nvs_row_t s_test_rows[TEST_ROW_CAP];

static bb_err_t run_iter(bb_diag_storage_nvs_snap_t *snap, size_t *out_row_count)
{
    size_t count = 0;
    bb_err_t rc = bb_diag_storage_nvs_iter(snap, NULL, 0, &count, NULL);
    if (rc != BB_OK) {
        *out_row_count = 0;
        return rc;
    }
    TEST_ASSERT_TRUE(count <= TEST_ROW_CAP);

    rc = bb_diag_storage_nvs_iter(snap, s_test_rows, count, &count, NULL);
    *out_row_count = count;
    return rc;
}

/* ---------------------------------------------------------------------------
 * Fixtures
 * ---------------------------------------------------------------------------*/
static const bb_storage_entry_t s_entries_basic[] = {
    { .ns_or_dir = "bb_cfg",       .key = "wifi_ssid",      .enc = BB_STORAGE_ENC_STR,  .len = 4  },
    { .ns_or_dir = "bb_cfg",       .key = "hostname",       .enc = BB_STORAGE_ENC_STR,  .len = 3  },
    { .ns_or_dir = "nvs.net80211", .key = "ap.sta.hostname", .enc = BB_STORAGE_ENC_BLOB, .len = 16 },
    { .ns_or_dir = "phy",          .key = "cal_data",       .enc = BB_STORAGE_ENC_BLOB, .len = 32 },
    { .ns_or_dir = "phy_extra",    .key = "foo",            .enc = BB_STORAGE_ENC_BLOB, .len = 8  },
    { .ns_or_dir = "bb_mqtt",      .key = "broker",         .enc = BB_STORAGE_ENC_STR,  .len = 12 },
};
#define S_ENTRIES_BASIC_N (sizeof(s_entries_basic) / sizeof(s_entries_basic[0]))

/* ---------------------------------------------------------------------------
 * Match/no-match against the bb_settings schema overlay
 * ---------------------------------------------------------------------------*/
void test_bb_diag_storage_nvs_iter_matches_schema_for_known_key(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_basic, S_ENTRIES_BASIC_N);

    bb_diag_storage_nvs_snap_t snap;
    size_t row_count = 0;
    TEST_ASSERT_EQUAL(BB_OK, run_iter(&snap, &row_count));

    const bb_diag_storage_nvs_row_t *row = &s_test_rows[0];
    TEST_ASSERT_EQUAL_STRING("bb_cfg", row->ns_or_dir);
    TEST_ASSERT_EQUAL_STRING("wifi_ssid", row->key);
    TEST_ASSERT_TRUE(row->has_schema);
    TEST_ASSERT_EQUAL_STRING_LEN("str", row->type_str.ptr, row->type_str.len);
    TEST_ASSERT_EQUAL_STRING_LEN("WiFi SSID", row->label.ptr, row->label.len);
    TEST_ASSERT_FALSE(row->secret);
    TEST_ASSERT_TRUE(row->provisioning_only);
    TEST_ASSERT_TRUE(row->reboot_required);
    TEST_ASSERT_FALSE(row->system);
}

// Proves the merge is per-KEY, not per-namespace: "hostname" shares
// "bb_cfg" with "wifi_ssid" but is not itself an overlay key.
void test_bb_diag_storage_nvs_iter_no_match_same_namespace(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_basic, S_ENTRIES_BASIC_N);

    bb_diag_storage_nvs_snap_t snap;
    size_t row_count = 0;
    TEST_ASSERT_EQUAL(BB_OK, run_iter(&snap, &row_count));

    const bb_diag_storage_nvs_row_t *row = &s_test_rows[1];
    TEST_ASSERT_EQUAL_STRING("bb_cfg", row->ns_or_dir);
    TEST_ASSERT_EQUAL_STRING("hostname", row->key);
    TEST_ASSERT_FALSE(row->has_schema);
    TEST_ASSERT_EQUAL_STRING_LEN("str", row->type_str.ptr, row->type_str.len);
    TEST_ASSERT_NULL(row->label.ptr);
    TEST_ASSERT_EQUAL_UINT(0, row->label.len);
    TEST_ASSERT_FALSE(row->secret);
    TEST_ASSERT_FALSE(row->provisioning_only);
    TEST_ASSERT_FALSE(row->reboot_required);
}

/* ---------------------------------------------------------------------------
 * ESP-system-namespace denylist -- exact match, not prefix
 * ---------------------------------------------------------------------------*/
void test_bb_diag_storage_nvs_iter_flags_net80211_system(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_basic, S_ENTRIES_BASIC_N);

    bb_diag_storage_nvs_snap_t snap;
    size_t row_count = 0;
    TEST_ASSERT_EQUAL(BB_OK, run_iter(&snap, &row_count));

    TEST_ASSERT_TRUE(s_test_rows[2].system);
}

void test_bb_diag_storage_nvs_iter_flags_phy_system(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_basic, S_ENTRIES_BASIC_N);

    bb_diag_storage_nvs_snap_t snap;
    size_t row_count = 0;
    TEST_ASSERT_EQUAL(BB_OK, run_iter(&snap, &row_count));

    TEST_ASSERT_TRUE(s_test_rows[3].system);
}

// "phy_extra" must NOT match "phy" -- exact string-equal, never a prefix.
void test_bb_diag_storage_nvs_iter_phy_extra_not_system(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_basic, S_ENTRIES_BASIC_N);

    bb_diag_storage_nvs_snap_t snap;
    size_t row_count = 0;
    TEST_ASSERT_EQUAL(BB_OK, run_iter(&snap, &row_count));

    TEST_ASSERT_FALSE(s_test_rows[4].system);
}

void test_bb_diag_storage_nvs_iter_plain_entry_not_system_not_schema(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_basic, S_ENTRIES_BASIC_N);

    bb_diag_storage_nvs_snap_t snap;
    size_t row_count = 0;
    TEST_ASSERT_EQUAL(BB_OK, run_iter(&snap, &row_count));

    const bb_diag_storage_nvs_row_t *row = &s_test_rows[5];
    TEST_ASSERT_FALSE(row->system);
    TEST_ASSERT_FALSE(row->has_schema);
    TEST_ASSERT_EQUAL_STRING_LEN("str", row->type_str.ptr, row->type_str.len);
}

/* ---------------------------------------------------------------------------
 * Stats + row/entry counts
 * ---------------------------------------------------------------------------*/
void test_bb_diag_storage_nvs_iter_stats_populated(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_basic, S_ENTRIES_BASIC_N);

    bb_diag_storage_nvs_snap_t snap;
    size_t row_count = 0;
    TEST_ASSERT_EQUAL(BB_OK, run_iter(&snap, &row_count));

    TEST_ASSERT_EQUAL_UINT64(100, snap.used_bytes);
    TEST_ASSERT_EQUAL_UINT64(900, snap.free_bytes);
    TEST_ASSERT_EQUAL_UINT64(1000, snap.total_bytes);
    TEST_ASSERT_EQUAL_UINT64(4, snap.namespace_count);
    TEST_ASSERT_EQUAL_UINT64(S_ENTRIES_BASIC_N, snap.entry_count);
    TEST_ASSERT_EQUAL_UINT(S_ENTRIES_BASIC_N, row_count);
}

/* ---------------------------------------------------------------------------
 * No-truncation -- BB_ARR_STREAM carries no fixed row-count cap; a fixture
 * well past the OLD fixed-cap default (16) round-trips every row.
 * ---------------------------------------------------------------------------*/
static bb_storage_entry_t s_entries_large[40];

static void build_large_fixture(void)
{
    for (size_t i = 0; i < 40; i++) {
        s_entries_large[i].ns_or_dir[0] = '\0';
        strncpy(s_entries_large[i].ns_or_dir, "bb_mqtt", sizeof(s_entries_large[i].ns_or_dir) - 1);
        s_entries_large[i].ns_or_dir[sizeof(s_entries_large[i].ns_or_dir) - 1] = '\0';
        snprintf(s_entries_large[i].key, sizeof(s_entries_large[i].key), "k%02u", (unsigned)i);
        s_entries_large[i].enc = BB_STORAGE_ENC_BLOB;
        s_entries_large[i].len = i;
    }
}

void test_bb_diag_storage_nvs_iter_no_truncation_at_40_rows(void)
{
    reset_all();
    register_fake_nvs();
    build_large_fixture();
    set_fixture(s_entries_large, 40);

    bb_diag_storage_nvs_snap_t snap;
    size_t row_count = 0;
    TEST_ASSERT_EQUAL(BB_OK, run_iter(&snap, &row_count));

    TEST_ASSERT_EQUAL_UINT64(40, snap.entry_count);
    TEST_ASSERT_EQUAL_UINT(40, row_count);
    for (size_t i = 0; i < 40; i++) {
        char expected_key[16];
        snprintf(expected_key, sizeof(expected_key), "k%02u", (unsigned)i);
        TEST_ASSERT_EQUAL_STRING(expected_key, s_test_rows[i].key);
        TEST_ASSERT_EQUAL_UINT64(i, s_test_rows[i].len);
    }
}

void test_bb_diag_storage_nvs_iter_empty_backend_is_stats_only(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(NULL, 0);

    bb_diag_storage_nvs_snap_t snap;
    size_t row_count = 0;
    TEST_ASSERT_EQUAL(BB_OK, run_iter(&snap, &row_count));

    TEST_ASSERT_EQUAL_UINT64(0, snap.entry_count);
    TEST_ASSERT_EQUAL_UINT(0, row_count);
    TEST_ASSERT_EQUAL_UINT64(100, snap.used_bytes);
}

/* ---------------------------------------------------------------------------
 * get_stats/list_entries BB_ERR_UNSUPPORTED passthrough
 * ---------------------------------------------------------------------------*/
void test_bb_diag_storage_nvs_iter_unsupported_backend_passthrough(void)
{
    reset_all();
    register_fake_nvs_no_enum();

    bb_diag_storage_nvs_snap_t snap;
    size_t row_count = 0;
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, run_iter(&snap, &row_count));
}

// list_entries succeeds (>=1 entry), get_stats fails -- distinct branch from
// the "unsupported backend" test above, which never reaches get_stats at all
// because list_entries short-circuits first.
void test_bb_diag_storage_nvs_iter_get_stats_fails_after_list_entries_ok(void)
{
    reset_all();
    register_fake_nvs_stats_fails();
    set_fixture(s_entries_basic, S_ENTRIES_BASIC_N);

    bb_diag_storage_nvs_snap_t snap;
    memset(&snap, 0xAA, sizeof(snap));
    size_t row_count = 0xAA;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_diag_storage_nvs_iter(&snap, NULL, 0, &row_count, NULL));

    // snap was zeroed at fn entry then never populated past the get_stats
    // failure -- confirm no partial stats leak through.
    TEST_ASSERT_EQUAL_UINT64(0, snap.used_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, snap.free_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, snap.total_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, snap.namespace_count);
    TEST_ASSERT_EQUAL_UINT64(0, snap.entry_count);
}

/* ---------------------------------------------------------------------------
 * Phase-2-only failure modes -- distinct branches from phase 1's own
 * list_entries/get_stats checks above.
 * ---------------------------------------------------------------------------*/

// A backend whose enumeration succeeds at phase 1 (COUNT) but fails on its
// SECOND call (phase 2, FILL) -- proves phase 2 propagates its own
// list_entries() failure rather than assuming success because phase 1 did.
void test_bb_diag_storage_nvs_iter_phase2_list_entries_failure_propagates(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_basic, S_ENTRIES_BASIC_N);
    // Each iter() call (both phases) makes its own unbounded cap=0
    // list_entries() probe up front (to re-derive entry_count/stats), plus
    // phase 2 makes a SECOND, bounded call for the actual rows -- so the
    // call sequence across TWO iter() invocations is: [1] phase-1's probe,
    // [2] phase-2's own probe, [3] phase-2's bounded fetch. Fail on call #3
    // to hit phase 2's OWN list_entries() error check specifically (lines
    // distinct from either probe's -- see bb_diag_storage_nvs_iter()).
    s_list_entries_fail_on_call = 3;

    bb_diag_storage_nvs_snap_t snap;
    size_t row_count = 0;
    bb_err_t rc = bb_diag_storage_nvs_iter(&snap, NULL, 0, &row_count, NULL);
    TEST_ASSERT_EQUAL(BB_OK, rc);
    TEST_ASSERT_EQUAL_UINT64(S_ENTRIES_BASIC_N, row_count);

    rc = bb_diag_storage_nvs_iter(&snap, s_test_rows, row_count, &row_count, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, rc);
}

static void *fail_alloc(size_t sz) { (void)sz; return NULL; }

// Phase 2's internal staging allocation (bb_malloc_prefer_spiram) failing
// closed -- BB_ERR_NO_MEM, no partial/garbage rows written into the caller's
// arena.
void test_bb_diag_storage_nvs_iter_phase2_staging_alloc_failure_returns_no_mem(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_basic, S_ENTRIES_BASIC_N);

    bb_diag_storage_nvs_snap_t snap;
    size_t row_count = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_nvs_iter(&snap, NULL, 0, &row_count, NULL));
    TEST_ASSERT_EQUAL_UINT64(S_ENTRIES_BASIC_N, row_count);

    memset(s_test_rows, 0xAA, sizeof(s_test_rows));
    bb_mem_set_alloc_hook(fail_alloc);
    bb_err_t rc = bb_diag_storage_nvs_iter(&snap, s_test_rows, row_count, &row_count, NULL);
    bb_mem_set_alloc_hook(NULL);

    TEST_ASSERT_EQUAL(BB_ERR_NO_MEM, rc);
    // Arena untouched -- still the 0xAA canary, never partially written.
    TEST_ASSERT_EQUAL_HEX8(0xAA, ((const uint8_t *)s_test_rows)[0]);
}

/* ---------------------------------------------------------------------------
 * enc_name() fallback -- scalar encodings (no schema match) render their
 * own type name, and an out-of-range bb_storage_enc_t value falls back to
 * "blob" (s_enc_names[] out-of-bounds guard).
 * ---------------------------------------------------------------------------*/
static const bb_storage_entry_t s_entries_enc_names[] = {
    { .ns_or_dir = "bb_app", .key = "retry_count", .enc = BB_STORAGE_ENC_U32, .len = 4 },
    { .ns_or_dir = "bb_app", .key = "flags",       .enc = BB_STORAGE_ENC_U8,  .len = 1 },
    { .ns_or_dir = "bb_app", .key = "unknown_enc", .enc = (bb_storage_enc_t)99, .len = 1 },
};
#define S_ENTRIES_ENC_NAMES_N (sizeof(s_entries_enc_names) / sizeof(s_entries_enc_names[0]))

void test_bb_diag_storage_nvs_iter_enc_name_scalar_types(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_enc_names, S_ENTRIES_ENC_NAMES_N);

    bb_diag_storage_nvs_snap_t snap;
    size_t row_count = 0;
    TEST_ASSERT_EQUAL(BB_OK, run_iter(&snap, &row_count));

    TEST_ASSERT_FALSE(s_test_rows[0].has_schema);
    TEST_ASSERT_EQUAL_STRING_LEN("u32", s_test_rows[0].type_str.ptr, s_test_rows[0].type_str.len);

    TEST_ASSERT_FALSE(s_test_rows[1].has_schema);
    TEST_ASSERT_EQUAL_STRING_LEN("u8", s_test_rows[1].type_str.ptr, s_test_rows[1].type_str.len);
}

void test_bb_diag_storage_nvs_iter_enc_name_out_of_range_falls_back_to_blob(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_enc_names, S_ENTRIES_ENC_NAMES_N);

    bb_diag_storage_nvs_snap_t snap;
    size_t row_count = 0;
    TEST_ASSERT_EQUAL(BB_OK, run_iter(&snap, &row_count));

    const bb_diag_storage_nvs_row_t *row = &s_test_rows[2];
    TEST_ASSERT_FALSE(row->has_schema);
    TEST_ASSERT_EQUAL_STRING_LEN("blob", row->type_str.ptr, row->type_str.len);
}

/* ---------------------------------------------------------------------------
 * Argument validation
 * ---------------------------------------------------------------------------*/
void test_bb_diag_storage_nvs_iter_null_dst_returns_invalid_arg(void)
{
    reset_all();
    register_fake_nvs();
    size_t row_count = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_storage_nvs_iter(NULL, NULL, 0, &row_count, NULL));
}

void test_bb_diag_storage_nvs_iter_null_row_count_returns_invalid_arg(void)
{
    reset_all();
    register_fake_nvs();
    bb_diag_storage_nvs_snap_t snap;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_storage_nvs_iter(&snap, NULL, 0, NULL, NULL));
}

/* ---------------------------------------------------------------------------
 * CONFIG_BB_OPENAPI_RUNTIME_META OFF (this env's default -- undefined) --
 * proves the describe route's 200-response schema is still the UNCHANGED
 * const literal path, byte- and pointer-identical to bb_diag_storage_nvs_
 * schema (both assigned from the SAME BB_DIAG_STORAGE_NVS_SCHEMA_LITERAL
 * macro invocation in bb_diag_storage_nvs.c -- see that file's `const
 * char *const bb_diag_storage_nvs_schema` doc comment for why that
 * identity holds). B1-1059 PR-2 pilot: config-OFF is a zero-diff no-op.
 * ---------------------------------------------------------------------------*/
void test_bb_diag_storage_nvs_describe_schema_is_unchanged_const_literal(void)
{
    // The for-test assemble is a documented no-op at this config gate (see
    // bb_diag_storage_nvs_test.h) -- still exercised here so the compiled-
    // out #else arm of bb_diag_storage_nvs_assemble_schema_for_test() is
    // covered, not just the accessor.
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_nvs_assemble_schema_for_test());

    TEST_ASSERT_EQUAL_PTR(bb_diag_storage_nvs_schema,
                           bb_diag_storage_nvs_get_describe_schema_for_test());
}

/* ---------------------------------------------------------------------------
 * Registration fits the shared scratch buffer -- turns the "confirm the
 * (now much smaller, no fixed-cap-array) snapshot fits" requirement into an
 * actual regression test, and proves the section registers as an `iter`
 * section (exactly one BB_ARR_STREAM field, elem_type OBJ, elem_size within
 * BB_SERIALIZE_MAX_ROW_BYTES).
 * ---------------------------------------------------------------------------*/
void test_bb_diag_storage_nvs_desc_fits_scratch(void)
{
    bb_diag_section_test_reset();

    bb_diag_section_t section = {
        .name         = "storage/nvs",
        .desc         = "test",
        .snap_desc    = &bb_diag_storage_nvs_desc,
        .iter         = bb_diag_storage_nvs_iter,
        .ctx          = NULL,
        .query_keys   = NULL,
        .n_query_keys = 0,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&section));

    bb_diag_section_test_reset();
}
