// Tests for bb_diag_storage_nvs -- exercises bb_diag_storage_nvs_fill()
// (the exact production code path, no mirror) against a fake "nvs"
// bb_storage backend (mirrors test_bb_storage.c's fake-backend idiom) and
// the REAL bb_settings_nv_overlay_entries() (host-portable schema source).
#include "unity.h"
#include "bb_diag_storage_nvs.h"
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
    *count = s_fixture_count;
    size_t n = s_fixture_count < cap ? s_fixture_count : cap;
    for (size_t i = 0; i < n; i++) out[i] = s_fixture[i];
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
void test_bb_diag_storage_nvs_fill_matches_schema_for_known_key(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_basic, S_ENTRIES_BASIC_N);

    bb_diag_storage_nvs_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_nvs_fill(&snap, NULL));

    const bb_diag_storage_nvs_row_t *row = &snap.entries_items[0];
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
void test_bb_diag_storage_nvs_fill_no_match_same_namespace(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_basic, S_ENTRIES_BASIC_N);

    bb_diag_storage_nvs_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_nvs_fill(&snap, NULL));

    const bb_diag_storage_nvs_row_t *row = &snap.entries_items[1];
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
void test_bb_diag_storage_nvs_fill_flags_net80211_system(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_basic, S_ENTRIES_BASIC_N);

    bb_diag_storage_nvs_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_nvs_fill(&snap, NULL));

    TEST_ASSERT_TRUE(snap.entries_items[2].system);
}

void test_bb_diag_storage_nvs_fill_flags_phy_system(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_basic, S_ENTRIES_BASIC_N);

    bb_diag_storage_nvs_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_nvs_fill(&snap, NULL));

    TEST_ASSERT_TRUE(snap.entries_items[3].system);
}

// "phy_extra" must NOT match "phy" -- exact string-equal, never a prefix.
void test_bb_diag_storage_nvs_fill_phy_extra_not_system(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_basic, S_ENTRIES_BASIC_N);

    bb_diag_storage_nvs_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_nvs_fill(&snap, NULL));

    TEST_ASSERT_FALSE(snap.entries_items[4].system);
}

void test_bb_diag_storage_nvs_fill_plain_entry_not_system_not_schema(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_basic, S_ENTRIES_BASIC_N);

    bb_diag_storage_nvs_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_nvs_fill(&snap, NULL));

    const bb_diag_storage_nvs_row_t *row = &snap.entries_items[5];
    TEST_ASSERT_FALSE(row->system);
    TEST_ASSERT_FALSE(row->has_schema);
    TEST_ASSERT_EQUAL_STRING_LEN("str", row->type_str.ptr, row->type_str.len);
}

/* ---------------------------------------------------------------------------
 * Stats + row/entry counts
 * ---------------------------------------------------------------------------*/
void test_bb_diag_storage_nvs_fill_stats_populated(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_basic, S_ENTRIES_BASIC_N);

    bb_diag_storage_nvs_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_nvs_fill(&snap, NULL));

    TEST_ASSERT_EQUAL_UINT64(100, snap.used_bytes);
    TEST_ASSERT_EQUAL_UINT64(900, snap.free_bytes);
    TEST_ASSERT_EQUAL_UINT64(1000, snap.total_bytes);
    TEST_ASSERT_EQUAL_UINT64(4, snap.namespace_count);
    TEST_ASSERT_EQUAL_UINT64(S_ENTRIES_BASIC_N, snap.entry_count);
    TEST_ASSERT_EQUAL_UINT(S_ENTRIES_BASIC_N, snap.entries.count);
    TEST_ASSERT_EQUAL_PTR(snap.entries_items, snap.entries.items);
}

/* ---------------------------------------------------------------------------
 * Truncation -- entry_count reports the TRUE total even when it exceeds
 * BB_DIAG_STORAGE_NVS_ENTRY_CAP; entries.count is capped.
 * ---------------------------------------------------------------------------*/
static bb_storage_entry_t s_entries_overflow[BB_DIAG_STORAGE_NVS_ENTRY_CAP + 4];

static void build_overflow_fixture(void)
{
    for (size_t i = 0; i < BB_DIAG_STORAGE_NVS_ENTRY_CAP + 4; i++) {
        s_entries_overflow[i].ns_or_dir[0] = '\0';
        strncpy(s_entries_overflow[i].ns_or_dir, "bb_mqtt", sizeof(s_entries_overflow[i].ns_or_dir) - 1);
        s_entries_overflow[i].ns_or_dir[sizeof(s_entries_overflow[i].ns_or_dir) - 1] = '\0';
        snprintf(s_entries_overflow[i].key, sizeof(s_entries_overflow[i].key), "k%02u", (unsigned)i);
        s_entries_overflow[i].enc = BB_STORAGE_ENC_BLOB;
        s_entries_overflow[i].len = 1;
    }
}

void test_bb_diag_storage_nvs_fill_truncates_when_over_cap(void)
{
    reset_all();
    register_fake_nvs();
    build_overflow_fixture();
    set_fixture(s_entries_overflow, BB_DIAG_STORAGE_NVS_ENTRY_CAP + 4);

    bb_diag_storage_nvs_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_nvs_fill(&snap, NULL));

    TEST_ASSERT_EQUAL_UINT64(BB_DIAG_STORAGE_NVS_ENTRY_CAP + 4, snap.entry_count);
    TEST_ASSERT_EQUAL_UINT(BB_DIAG_STORAGE_NVS_ENTRY_CAP, snap.entries.count);
}

void test_bb_diag_storage_nvs_fill_empty_backend_is_stats_only(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(NULL, 0);

    bb_diag_storage_nvs_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_nvs_fill(&snap, NULL));

    TEST_ASSERT_EQUAL_UINT64(0, snap.entry_count);
    TEST_ASSERT_EQUAL_UINT(0, snap.entries.count);
    TEST_ASSERT_EQUAL_UINT64(100, snap.used_bytes);
}

/* ---------------------------------------------------------------------------
 * get_stats/list_entries BB_ERR_UNSUPPORTED passthrough
 * ---------------------------------------------------------------------------*/
void test_bb_diag_storage_nvs_fill_unsupported_backend_passthrough(void)
{
    reset_all();
    register_fake_nvs_no_enum();

    bb_diag_storage_nvs_snap_t snap;
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_diag_storage_nvs_fill(&snap, NULL));
}

// list_entries succeeds (>=1 entry), get_stats fails -- distinct branch from
// the "unsupported backend" test above, which never reaches get_stats at all
// because list_entries short-circuits first.
void test_bb_diag_storage_nvs_fill_get_stats_fails_after_list_entries_ok(void)
{
    reset_all();
    register_fake_nvs_stats_fails();
    set_fixture(s_entries_basic, S_ENTRIES_BASIC_N);

    bb_diag_storage_nvs_snap_t snap;
    memset(&snap, 0xAA, sizeof(snap));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_diag_storage_nvs_fill(&snap, NULL));

    // snap was zeroed at fn entry then never populated past the get_stats
    // failure -- confirm no partial stats leak through.
    TEST_ASSERT_EQUAL_UINT64(0, snap.used_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, snap.free_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, snap.total_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, snap.namespace_count);
    TEST_ASSERT_EQUAL_UINT(0, snap.entries.count);
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

void test_bb_diag_storage_nvs_fill_enc_name_scalar_types(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_enc_names, S_ENTRIES_ENC_NAMES_N);

    bb_diag_storage_nvs_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_nvs_fill(&snap, NULL));

    TEST_ASSERT_FALSE(snap.entries_items[0].has_schema);
    TEST_ASSERT_EQUAL_STRING_LEN("u32", snap.entries_items[0].type_str.ptr, snap.entries_items[0].type_str.len);

    TEST_ASSERT_FALSE(snap.entries_items[1].has_schema);
    TEST_ASSERT_EQUAL_STRING_LEN("u8", snap.entries_items[1].type_str.ptr, snap.entries_items[1].type_str.len);
}

void test_bb_diag_storage_nvs_fill_enc_name_out_of_range_falls_back_to_blob(void)
{
    reset_all();
    register_fake_nvs();
    set_fixture(s_entries_enc_names, S_ENTRIES_ENC_NAMES_N);

    bb_diag_storage_nvs_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_storage_nvs_fill(&snap, NULL));

    const bb_diag_storage_nvs_row_t *row = &snap.entries_items[2];
    TEST_ASSERT_FALSE(row->has_schema);
    TEST_ASSERT_EQUAL_STRING_LEN("blob", row->type_str.ptr, row->type_str.len);
}

void test_bb_diag_storage_nvs_fill_null_dst_returns_invalid_arg(void)
{
    reset_all();
    register_fake_nvs();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_diag_storage_nvs_fill(NULL, NULL));
}

/* ---------------------------------------------------------------------------
 * Registration fits the shared scratch buffer -- turns the "confirm both
 * split snapshots fit" requirement into an actual regression test.
 * ---------------------------------------------------------------------------*/
void test_bb_diag_storage_nvs_desc_fits_scratch(void)
{
    bb_diag_section_test_reset();

    bb_diag_section_t section = {
        .name         = "storage/nvs",
        .desc         = "test",
        .snap_desc    = &bb_diag_storage_nvs_desc,
        .fill         = bb_diag_storage_nvs_fill,
        .ctx          = NULL,
        .query_keys   = NULL,
        .n_query_keys = 0,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&section));

    bb_diag_section_test_reset();
}
