#include "unity.h"
#include "bb_storage.h"
#include "bb_storage_nvs.h"
#include "bb_storage_nvs_classify_nvs_type.h"
#include "bb_core.h"

#include <string.h>

// bb_storage_nvs's enumeration-primitive seam (bb_storage_nvs_entry_ops_t,
// BB_STORAGE_NVS_TESTING-gated): drives nvs_vt_list_entries/nvs_vt_get_stats's
// find->next->info->release iteration + value-length-probe orchestration
// against a fake, without any real NVS/ESP-IDF backing. Covers the loud-
// truncation + non-fatal-probe-failure + entry-mapping contract that only
// ever runs on esp32 otherwise (bb_storage_nvs_register() itself returns
// BB_ERR_UNSUPPORTED on host; this seam bypasses that facade layer entirely
// and drives the orchestration directly).

#define FAKE_ENTRY_MAX 8

typedef struct {
    char   ns[16];
    char   key[16];
    int    raw_type;
    size_t len;
    bool   probe_fail;
} fake_entry_t;

static fake_entry_t s_fake_entries[FAKE_ENTRY_MAX];
static int          s_fake_entry_count;
static int          s_iter_pos;

static const char *s_last_find_ns_or_dir;
static bool        s_last_find_ns_or_dir_was_null;
static int         s_find_calls, s_next_calls, s_info_calls, s_release_calls;
static int         s_open_calls, s_probe_calls, s_close_calls, s_stats_calls;
static bool         s_find_not_found;  // force an empty backend

// Fix-1 leak coverage: fake_next() can be told to return a genuine
// mid-iteration error (not BB_ERR_NOT_FOUND) at a specific iterator
// position, and release() tracks the handle it was called with so a test
// can assert the still-valid iterator is released exactly once.
static bool     s_next_error_at_pos_set;
static int      s_next_error_at_pos;
static bb_err_t s_next_error_value;
static uint32_t s_last_release_it;

static bool s_info_error_at_pos_set;
static int  s_info_error_at_pos;

static void fake_reset(void)
{
    memset(s_fake_entries, 0, sizeof(s_fake_entries));
    s_fake_entry_count             = 0;
    s_iter_pos                     = -1;
    s_last_find_ns_or_dir           = "unset";
    s_last_find_ns_or_dir_was_null  = false;
    s_find_calls = s_next_calls = s_info_calls = s_release_calls = 0;
    s_open_calls = s_probe_calls = s_close_calls = s_stats_calls  = 0;
    s_find_not_found                = false;
    s_next_error_at_pos_set          = false;
    s_next_error_at_pos              = 0;
    s_next_error_value               = BB_OK;
    s_last_release_it                = 0xffffffffu;
    s_info_error_at_pos_set          = false;
    s_info_error_at_pos              = 0;
}

static void fake_add_entry(const char *ns, const char *key, int raw_type, size_t len, bool probe_fail)
{
    fake_entry_t *e = &s_fake_entries[s_fake_entry_count++];
    strncpy(e->ns, ns, sizeof(e->ns) - 1);
    strncpy(e->key, key, sizeof(e->key) - 1);
    e->raw_type   = raw_type;
    e->len        = len;
    e->probe_fail = probe_fail;
}

static bb_err_t fake_find(const char *ns_or_dir, uint32_t *out_it)
{
    s_find_calls++;
    s_last_find_ns_or_dir          = ns_or_dir;
    s_last_find_ns_or_dir_was_null = (ns_or_dir == NULL);

    if (s_find_not_found || s_fake_entry_count == 0) {
        *out_it = 0;
        return BB_ERR_NOT_FOUND;
    }
    s_iter_pos = 0;
    *out_it    = 1;
    return BB_OK;
}

static bb_err_t fake_next(uint32_t *it)
{
    s_next_calls++;
    if (s_next_error_at_pos_set && s_next_calls == s_next_error_at_pos) {
        // Genuine mid-iteration error: per ESP-IDF's documented contract the
        // iterator is left unmodified (still a live handle the caller must
        // release) on any error other than exhaustion -- the fake mirrors
        // that by leaving *it pointing at the still-live handle.
        return s_next_error_value;
    }
    s_iter_pos++;
    if (s_iter_pos >= s_fake_entry_count) {
        *it = 0;
        return BB_ERR_NOT_FOUND;
    }
    *it = 1;
    return BB_OK;
}

static bb_err_t fake_info(uint32_t it, char *ns_out, char *key_out, int *raw_type_out)
{
    (void)it;
    s_info_calls++;
    if (s_info_error_at_pos_set && s_iter_pos == s_info_error_at_pos) {
        return BB_ERR_TIMEOUT;  // stand-in for a genuine nvs_entry_info() failure
    }
    fake_entry_t *e = &s_fake_entries[s_iter_pos];
    strncpy(ns_out, e->ns, 16);
    ns_out[15] = '\0';
    strncpy(key_out, e->key, 16);
    key_out[15] = '\0';
    *raw_type_out = e->raw_type;
    return BB_OK;
}

static void fake_release(uint32_t it)
{
    s_release_calls++;
    s_last_release_it = it;
}

static bb_err_t fake_open(const char *ns, uint32_t *out_handle)
{
    (void)ns;
    s_open_calls++;
    *out_handle = 42;
    return BB_OK;
}

static bb_err_t fake_probe_len(uint32_t handle, const char *key, int raw_type, size_t *out_len)
{
    (void)handle;
    (void)key;
    (void)raw_type;
    s_probe_calls++;
    fake_entry_t *e = &s_fake_entries[s_iter_pos];
    if (e->probe_fail) {
        return BB_ERR_NOT_FOUND;
    }
    *out_len = e->len;
    return BB_OK;
}

static void fake_close(uint32_t handle)
{
    (void)handle;
    s_close_calls++;
}

static bb_err_t fake_stats(size_t *used_entries, size_t *free_entries, size_t *total_entries)
{
    s_stats_calls++;
    *used_entries  = 10;
    *free_entries  = 20;
    *total_entries = 30;
    return BB_OK;
}

static const bb_storage_nvs_entry_ops_t s_fake_ops = {
    .find      = fake_find,
    .next      = fake_next,
    .info      = fake_info,
    .release   = fake_release,
    .open      = fake_open,
    .probe_len = fake_probe_len,
    .close     = fake_close,
    .stats     = fake_stats,
};

/* ---------------------------------------------------------------------------
 * 1. Entries pass through with correct ns/key/enc/len mapping.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_nvs_list_entries_maps_entries_enc_and_len(void)
{
    fake_reset();
    fake_add_entry("wifi", "ssid", BB_STORAGE_NVS_RAW_TYPE_STR, 4, false);
    fake_add_entry("wifi", "count", BB_STORAGE_NVS_RAW_TYPE_U32, 4, false);
    bb_storage_nvs_set_entry_ops_for_test(&s_fake_ops);

    bb_storage_entry_t out[4];
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_list_entries_for_test(NULL, out, 4, &count));
    TEST_ASSERT_EQUAL(2, count);

    TEST_ASSERT_EQUAL_STRING("wifi", out[0].ns_or_dir);
    TEST_ASSERT_EQUAL_STRING("ssid", out[0].key);
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_STR, out[0].enc);
    TEST_ASSERT_EQUAL(4, out[0].len);

    TEST_ASSERT_EQUAL_STRING("wifi", out[1].ns_or_dir);
    TEST_ASSERT_EQUAL_STRING("count", out[1].key);
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_U32, out[1].enc);
    TEST_ASSERT_EQUAL(4, out[1].len);

    bb_storage_nvs_set_entry_ops_for_test(NULL);
}

/* ---------------------------------------------------------------------------
 * 2. ns_or_dir == NULL reaches the fake's find() unchanged (the "all
 *    namespaces" convention lives at the NVS call site, not this layer).
 * ---------------------------------------------------------------------------*/
void test_bb_storage_nvs_list_entries_null_ns_or_dir_reaches_find_as_null(void)
{
    fake_reset();
    fake_add_entry("wifi", "ssid", BB_STORAGE_NVS_RAW_TYPE_STR, 4, false);
    bb_storage_nvs_set_entry_ops_for_test(&s_fake_ops);

    bb_storage_entry_t out[4];
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_list_entries_for_test(NULL, out, 4, &count));
    TEST_ASSERT_TRUE(s_last_find_ns_or_dir_was_null);

    bb_storage_nvs_set_entry_ops_for_test(NULL);
}

void test_bb_storage_nvs_list_entries_non_null_ns_or_dir_reaches_find_unchanged(void)
{
    fake_reset();
    fake_add_entry("wifi", "ssid", BB_STORAGE_NVS_RAW_TYPE_STR, 4, false);
    bb_storage_nvs_set_entry_ops_for_test(&s_fake_ops);

    bb_storage_entry_t out[4];
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_list_entries_for_test("wifi", out, 4, &count));
    TEST_ASSERT_FALSE(s_last_find_ns_or_dir_was_null);
    TEST_ASSERT_EQUAL_STRING("wifi", s_last_find_ns_or_dir);

    bb_storage_nvs_set_entry_ops_for_test(NULL);
}

/* ---------------------------------------------------------------------------
 * 3. Truncation is loud: *count reports the TRUE total even when it exceeds
 *    cap, and only the first `cap` entries are written to `out`.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_nvs_list_entries_truncates_loudly_reports_true_count(void)
{
    fake_reset();
    fake_add_entry("wifi", "ssid", BB_STORAGE_NVS_RAW_TYPE_STR, 4, false);
    fake_add_entry("wifi", "pass", BB_STORAGE_NVS_RAW_TYPE_STR, 8, false);
    fake_add_entry("cfg",  "mode", BB_STORAGE_NVS_RAW_TYPE_U8,  1, false);
    bb_storage_nvs_set_entry_ops_for_test(&s_fake_ops);

    bb_storage_entry_t out[1];
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_list_entries_for_test(NULL, out, 1, &count));
    TEST_ASSERT_EQUAL(3, count);  // true total, not capped
    TEST_ASSERT_EQUAL_STRING("ssid", out[0].key);  // only cap(1) entries written

    bb_storage_nvs_set_entry_ops_for_test(NULL);
}

/* ---------------------------------------------------------------------------
 * 4. A value-length probe failure is loud-but-non-fatal: that one entry
 *    reports len=0, the rest of the enumeration still completes.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_nvs_list_entries_probe_failure_reports_len_zero_not_abort(void)
{
    fake_reset();
    fake_add_entry("wifi", "ssid", BB_STORAGE_NVS_RAW_TYPE_STR, 4, true /* probe fails */);
    fake_add_entry("wifi", "pass", BB_STORAGE_NVS_RAW_TYPE_STR, 8, false);
    bb_storage_nvs_set_entry_ops_for_test(&s_fake_ops);

    bb_storage_entry_t out[4];
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_list_entries_for_test(NULL, out, 4, &count));
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL(0, out[0].len);   // probe failed -> len=0, not aborted
    TEST_ASSERT_EQUAL(8, out[1].len);   // second entry unaffected

    bb_storage_nvs_set_entry_ops_for_test(NULL);
}

/* ---------------------------------------------------------------------------
 * 5. An empty backend (find() -> NOT_FOUND) is a successful empty listing,
 *    not an error.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_nvs_list_entries_empty_backend_returns_ok_zero_count(void)
{
    fake_reset();
    s_find_not_found = true;
    bb_storage_nvs_set_entry_ops_for_test(&s_fake_ops);

    bb_storage_entry_t out[4];
    size_t count = 1;  // deliberately non-zero to prove it gets overwritten
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_list_entries_for_test(NULL, out, 4, &count));
    TEST_ASSERT_EQUAL(0, count);

    bb_storage_nvs_set_entry_ops_for_test(NULL);
}

/* ---------------------------------------------------------------------------
 * 6. No ops injected -> fails closed, never a crash.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_nvs_list_entries_unset_ops_returns_unsupported(void)
{
    fake_reset();
    bb_storage_nvs_set_entry_ops_for_test(NULL);

    bb_storage_entry_t out[4];
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_storage_nvs_list_entries_for_test(NULL, out, 4, &count));
}

void test_bb_storage_nvs_get_stats_unset_ops_returns_unsupported(void)
{
    fake_reset();
    bb_storage_nvs_set_entry_ops_for_test(NULL);

    bb_storage_stats_t out;
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_storage_nvs_get_stats_for_test(&out));
}

/* ---------------------------------------------------------------------------
 * 7. get_stats maps NVS entry counts to bytes via the documented *32
 *    entry-slot-size conversion (NVS-entry-based, not exact partition-byte
 *    accounting -- see bb_storage_stats_t's field comment in bb_storage.h).
 * ---------------------------------------------------------------------------*/
void test_bb_storage_nvs_get_stats_maps_entries_to_bytes_times_32(void)
{
    fake_reset();
    bb_storage_nvs_set_entry_ops_for_test(&s_fake_ops);

    bb_storage_stats_t out;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_get_stats_for_test(&out));
    TEST_ASSERT_EQUAL(1, s_stats_calls);
    TEST_ASSERT_EQUAL(320, out.used_bytes);   // 10 * 32
    TEST_ASSERT_EQUAL(640, out.free_bytes);   // 20 * 32
    TEST_ASSERT_EQUAL(960, out.total_bytes);  // 30 * 32
    TEST_ASSERT_EQUAL(0, out.namespace_count);  // not cheaply available

    bb_storage_nvs_set_entry_ops_for_test(NULL);
}

/* ---------------------------------------------------------------------------
 * 8. A genuine mid-iteration next() error (not BB_ERR_NOT_FOUND) must
 *    propagate to the caller AND the still-valid (non-zero) iterator handle
 *    must be released exactly once -- this test exercises the orchestration's
 *    release-once contract-handling (nvs_vt_list_entries via the fake `next`
 *    op). Real ESP-IDF leaves iterators unmodified on errors other than
 *    exhaustion (ESP_ERR_NVS_NOT_FOUND), and the fake mirrors that contract
 *    so we can validate the orchestration's release(it) call with a live
 *    handle. Note: entry_next_real itself is device-only (#ifdef ESP_PLATFORM),
 *    not executed by this host test -- validated by code review + smoke/HW only
 *    (B1-943 class).
 * ---------------------------------------------------------------------------*/
void test_bb_storage_nvs_list_entries_genuine_next_error_releases_live_iterator_once(void)
{
    fake_reset();
    fake_add_entry("wifi", "ssid", BB_STORAGE_NVS_RAW_TYPE_STR, 4, false);
    fake_add_entry("wifi", "pass", BB_STORAGE_NVS_RAW_TYPE_STR, 8, false);
    s_next_error_at_pos_set = true;
    s_next_error_at_pos     = 1;  // fail on the first next() call
    s_next_error_value      = BB_ERR_TIMEOUT;
    bb_storage_nvs_set_entry_ops_for_test(&s_fake_ops);

    bb_storage_entry_t out[4];
    size_t count = 0;
    bb_err_t result = bb_storage_nvs_list_entries_for_test(NULL, out, 4, &count);

    TEST_ASSERT_EQUAL(BB_ERR_TIMEOUT, result);
    TEST_ASSERT_EQUAL(1, s_release_calls);
    TEST_ASSERT_NOT_EQUAL(0, s_last_release_it);  // still-valid handle, not zeroed

    bb_storage_nvs_set_entry_ops_for_test(NULL);
}

/* ---------------------------------------------------------------------------
 * 9. A nvs_entry_info() failure is loud-but-non-fatal: that one entry is
 *    skipped (not written to `out`, not counted), enumeration continues, and
 *    the rest of the entries still come through.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_nvs_list_entries_info_failure_skips_entry_continues(void)
{
    fake_reset();
    fake_add_entry("wifi", "ssid", BB_STORAGE_NVS_RAW_TYPE_STR, 4, false);
    fake_add_entry("wifi", "bad",  BB_STORAGE_NVS_RAW_TYPE_STR, 4, false);
    fake_add_entry("wifi", "pass", BB_STORAGE_NVS_RAW_TYPE_STR, 8, false);
    s_info_error_at_pos_set = true;
    s_info_error_at_pos     = 1;  // the "bad" entry's info() call fails
    bb_storage_nvs_set_entry_ops_for_test(&s_fake_ops);

    bb_storage_entry_t out[4];
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_nvs_list_entries_for_test(NULL, out, 4, &count));
    TEST_ASSERT_EQUAL(2, count);  // "bad" entry skipped, not counted
    TEST_ASSERT_EQUAL_STRING("ssid", out[0].key);
    TEST_ASSERT_EQUAL_STRING("pass", out[1].key);

    bb_storage_nvs_set_entry_ops_for_test(NULL);
}
