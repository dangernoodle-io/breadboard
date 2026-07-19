#include "unity.h"
#include "bb_storage.h"
#include "bb_storage_ram.h"
#include "bb_core.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------*/
static void reset_all(void)
{
    bb_storage_test_reset();
    bb_storage_ram_test_reset();
}

static const bb_storage_addr_t ADDR_FOO = {
    .backend   = "ram",
    .ns_or_dir = NULL,
    .key       = "foo",
};

static const bb_storage_addr_t ADDR_UNKNOWN_BACKEND = {
    .backend   = "does_not_exist",
    .ns_or_dir = NULL,
    .key       = "foo",
};

/* ---------------------------------------------------------------------------
 * set/get round-trip
 * ---------------------------------------------------------------------------*/
void test_bb_storage_ram_set_get_round_trip(void)
{
    reset_all();
    bb_storage_ram_register();

    const char *val = "hello";
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&ADDR_FOO, val, strlen(val)));

    char buf[16] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&ADDR_FOO, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(strlen(val), out_len);
    TEST_ASSERT_EQUAL_STRING_LEN(val, buf, out_len);
}

/* ---------------------------------------------------------------------------
 * erase
 * ---------------------------------------------------------------------------*/
void test_bb_storage_ram_erase_removes_value(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&ADDR_FOO, "x", 1));
    TEST_ASSERT_TRUE(bb_storage_exists(&ADDR_FOO));

    TEST_ASSERT_EQUAL(BB_OK, bb_storage_erase(&ADDR_FOO));
    TEST_ASSERT_FALSE(bb_storage_exists(&ADDR_FOO));

    size_t out_len = 0;
    char buf[4];
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_get(&ADDR_FOO, buf, sizeof(buf), &out_len));
}

/* ---------------------------------------------------------------------------
 * erase is idempotent on a missing key
 * ---------------------------------------------------------------------------*/
void test_bb_storage_ram_erase_missing_key_is_ok(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_OK, bb_storage_erase(&ADDR_FOO));
}

/* ---------------------------------------------------------------------------
 * exists
 * ---------------------------------------------------------------------------*/
void test_bb_storage_exists_true_after_set_false_before(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_FALSE(bb_storage_exists(&ADDR_FOO));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&ADDR_FOO, "y", 1));
    TEST_ASSERT_TRUE(bb_storage_exists(&ADDR_FOO));
}

void test_bb_storage_exists_false_for_null_addr(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_FALSE(bb_storage_exists(NULL));
}

void test_bb_storage_exists_false_for_unknown_backend(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_FALSE(bb_storage_exists(&ADDR_UNKNOWN_BACKEND));
}

/* ---------------------------------------------------------------------------
 * unknown backend errors
 * ---------------------------------------------------------------------------*/
void test_bb_storage_get_unknown_backend_returns_not_found(void)
{
    reset_all();
    bb_storage_ram_register();

    char buf[4];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_get(&ADDR_UNKNOWN_BACKEND, buf, sizeof(buf), &out_len));
}

void test_bb_storage_set_unknown_backend_returns_not_found(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_set(&ADDR_UNKNOWN_BACKEND, "z", 1));
}

void test_bb_storage_erase_unknown_backend_returns_not_found(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_erase(&ADDR_UNKNOWN_BACKEND));
}

/* ---------------------------------------------------------------------------
 * erase_namespace (B1-757): NULL args, unknown backend, and a backend whose
 * vtable leaves erase_namespace NULL (bb_storage_ram deliberately does, per
 * the facade's "NULL -> BB_ERR_UNSUPPORTED, never a silent no-op" contract).
 * The success path (a backend that DOES implement it) is exercised by
 * test_bb_storage_http_routes.c against its own fake "nvs"/"alt" backends.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_erase_namespace_null_backend_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_erase_namespace(NULL, "ns"));
}

void test_bb_storage_erase_namespace_null_ns_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_erase_namespace("ram", NULL));
}

void test_bb_storage_erase_namespace_unknown_backend_returns_not_found(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_erase_namespace("does_not_exist", "ns"));
}

void test_bb_storage_erase_namespace_ram_backend_returns_unsupported(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_storage_erase_namespace("ram", "ns"));
}

/* ---------------------------------------------------------------------------
 * erase_all (B1-960): NULL backend, unknown backend, and a backend whose
 * vtable leaves erase_all NULL (bb_storage_ram deliberately does, same
 * fail-closed contract as erase_namespace above). The success path (a
 * backend that DOES implement it) is exercised by
 * test_bb_storage_http_factory_reset.c against its own fake "nvs" backend.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_erase_all_null_backend_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_erase_all(NULL));
}

void test_bb_storage_erase_all_unknown_backend_returns_not_found(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_erase_all("does_not_exist"));
}

void test_bb_storage_erase_all_ram_backend_returns_unsupported(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_storage_erase_all("ram"));
}

/* ---------------------------------------------------------------------------
 * missing-key get (backend known, key never set)
 * ---------------------------------------------------------------------------*/
void test_bb_storage_get_missing_key_returns_not_found(void)
{
    reset_all();
    bb_storage_ram_register();

    char buf[4];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_get(&ADDR_FOO, buf, sizeof(buf), &out_len));
}

/* ---------------------------------------------------------------------------
 * buffer-too-small: out_len reports the real length, buf holds a prefix
 * ---------------------------------------------------------------------------*/
void test_bb_storage_get_buffer_too_small_reports_full_len(void)
{
    reset_all();
    bb_storage_ram_register();

    const char *val = "0123456789";
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&ADDR_FOO, val, strlen(val)));

    char buf[4] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&ADDR_FOO, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(strlen(val), out_len);
    TEST_ASSERT_EQUAL_STRING_LEN(val, buf, sizeof(buf));
}

/* ---------------------------------------------------------------------------
 * size-probe: cap=0 still reports out_len (no buf write)
 * ---------------------------------------------------------------------------*/
void test_bb_storage_get_zero_cap_probes_length(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&ADDR_FOO, "abcde", 5));

    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&ADDR_FOO, NULL, 0, &out_len));
    TEST_ASSERT_EQUAL(5, out_len);
}

/* ---------------------------------------------------------------------------
 * overwrite
 * ---------------------------------------------------------------------------*/
void test_bb_storage_set_overwrites_existing_key(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&ADDR_FOO, "first", 5));
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&ADDR_FOO, "second-value", 12));

    char buf[16] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&ADDR_FOO, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(12, out_len);
    TEST_ASSERT_EQUAL_STRING_LEN("second-value", buf, out_len);
}

/* ---------------------------------------------------------------------------
 * get/set/erase invalid-arg branches (facade-level validation)
 * ---------------------------------------------------------------------------*/
void test_bb_storage_get_null_addr_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    char buf[4];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_get(NULL, buf, sizeof(buf), &out_len));
}

void test_bb_storage_get_null_backend_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    bb_storage_addr_t addr = { .backend = NULL, .ns_or_dir = NULL, .key = "foo" };
    char buf[4];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_get(&addr, buf, sizeof(buf), &out_len));
}

void test_bb_storage_get_null_out_len_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    char buf[4];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_get(&ADDR_FOO, buf, sizeof(buf), NULL));
}

void test_bb_storage_get_null_buf_with_nonzero_cap_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_get(&ADDR_FOO, NULL, 4, &out_len));
}

void test_bb_storage_set_null_addr_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_set(NULL, "x", 1));
}

void test_bb_storage_set_null_buf_with_nonzero_len_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_set(&ADDR_FOO, NULL, 4));
}

void test_bb_storage_set_null_buf_zero_len_is_ok(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&ADDR_FOO, NULL, 0));
    TEST_ASSERT_TRUE(bb_storage_exists(&ADDR_FOO));

    size_t out_len = 123;
    char buf[4];
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&ADDR_FOO, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(0, out_len);
}

void test_bb_storage_erase_null_addr_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_erase(NULL));
}

/* ---------------------------------------------------------------------------
 * register_backend validation + registry semantics
 * ---------------------------------------------------------------------------*/
static bb_err_t stub_get(void *impl, const bb_storage_addr_t *addr, void *buf, size_t cap, size_t *out_len)
{
    (void)impl; (void)addr; (void)buf; (void)cap;
    *out_len = 0;
    return BB_OK;
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

void test_bb_storage_register_backend_null_name_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_vtable_t vt = { stub_get, stub_set, stub_erase, stub_exists };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_register_backend(NULL, &vt, NULL));
}

void test_bb_storage_register_backend_null_vtable_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_register_backend("x", NULL, NULL));
}

void test_bb_storage_register_backend_partial_vtable_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_vtable_t vt = { NULL, stub_set, stub_erase, stub_exists };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_register_backend("x", &vt, NULL));
}

void test_bb_storage_register_backend_duplicate_name_rejected(void)
{
    reset_all();
    bb_storage_vtable_t vt = { stub_get, stub_set, stub_erase, stub_exists };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_register_backend("dup", &vt, NULL));
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_storage_register_backend("dup", &vt, NULL));
}

void test_bb_storage_register_backend_overflow_returns_no_space(void)
{
    reset_all();
    bb_storage_vtable_t vt = { stub_get, stub_set, stub_erase, stub_exists };
    char names[BB_STORAGE_MAX_BACKENDS + 1][16];
    for (int i = 0; i < BB_STORAGE_MAX_BACKENDS; i++) {
        snprintf(names[i], sizeof(names[i]), "b%d", i);
        TEST_ASSERT_EQUAL(BB_OK, bb_storage_register_backend(names[i], &vt, NULL));
    }
    snprintf(names[BB_STORAGE_MAX_BACKENDS], sizeof(names[BB_STORAGE_MAX_BACKENDS]), "overflow");
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_storage_register_backend(names[BB_STORAGE_MAX_BACKENDS], &vt, NULL));
}

/* ---------------------------------------------------------------------------
 * list_entries/get_stats (B1-767 PR5): optional pair validation at
 * registration (mirrors get_typed/set_typed above), fail-closed
 * BB_ERR_UNSUPPORTED on a backend that leaves the pair NULL (bb_storage_ram
 * deliberately does), facade-level NULL-arg/unknown-backend guards, and a
 * fake backend proving entries/stats + the ns_or_dir==NULL "all namespaces"
 * convention + loud-truncation contract pass through unmodified.
 *
 * The fake backend's fns/state/register helper are defined first so the
 * pair-validation tests below can register a genuine list_entries/get_stats
 * fn ptr instead of an unsafe incompatible-cast stub.
 * ---------------------------------------------------------------------------*/
static const bb_storage_entry_t s_fake_entries[] = {
    { .ns_or_dir = "wifi", .key = "ssid", .enc = BB_STORAGE_ENC_STR, .len = 4 },
    { .ns_or_dir = "wifi", .key = "pass", .enc = BB_STORAGE_ENC_STR, .len = 8 },
    { .ns_or_dir = "cfg",  .key = "mode", .enc = BB_STORAGE_ENC_U8,  .len = 1 },
};
static const char *s_last_list_ns_or_dir;
static bool        s_last_list_ns_or_dir_was_null;

static bb_err_t fake_enum_list_entries(void *impl, const char *ns_or_dir, bb_storage_entry_t *out,
                                        size_t cap, size_t *count)
{
    (void)impl;
    s_last_list_ns_or_dir = ns_or_dir;
    s_last_list_ns_or_dir_was_null = (ns_or_dir == NULL);

    size_t total = sizeof(s_fake_entries) / sizeof(s_fake_entries[0]);
    *count = total;
    size_t n = (total < cap) ? total : cap;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_fake_entries[i];
    }
    return BB_OK;
}

static bb_err_t fake_enum_get_stats(void *impl, bb_storage_stats_t *out)
{
    (void)impl;
    out->used_bytes       = 128;
    out->free_bytes       = 896;
    out->total_bytes      = 1024;
    out->namespace_count  = 2;
    return BB_OK;
}

static void register_fake_enum_backend(void)
{
    s_last_list_ns_or_dir = "unset";
    s_last_list_ns_or_dir_was_null = false;
    bb_storage_vtable_t vt = {
        .get           = stub_get,
        .set           = stub_set,
        .erase         = stub_erase,
        .exists        = stub_exists,
        .list_entries  = fake_enum_list_entries,
        .get_stats     = fake_enum_get_stats,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_register_backend("fake_enum", &vt, NULL));
}

void test_bb_storage_register_backend_list_entries_without_get_stats_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_vtable_t vt = {
        .get = stub_get, .set = stub_set, .erase = stub_erase, .exists = stub_exists,
        .list_entries = fake_enum_list_entries,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_register_backend("partial_enum1", &vt, NULL));
}

void test_bb_storage_register_backend_get_stats_without_list_entries_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_vtable_t vt = {
        .get = stub_get, .set = stub_set, .erase = stub_erase, .exists = stub_exists,
        .get_stats = fake_enum_get_stats,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_register_backend("partial_enum2", &vt, NULL));
}

void test_bb_storage_register_backend_both_enum_null_succeeds(void)
{
    reset_all();
    bb_storage_vtable_t vt = {
        .get = stub_get, .set = stub_set, .erase = stub_erase, .exists = stub_exists,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_register_backend("no_enum", &vt, NULL));
}

void test_bb_storage_list_entries_null_backend_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    bb_storage_entry_t out[1];
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_list_entries(NULL, NULL, out, 1, &count));
}

void test_bb_storage_list_entries_null_count_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    bb_storage_entry_t out[1];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_list_entries("ram", NULL, out, 1, NULL));
}

void test_bb_storage_list_entries_null_out_with_nonzero_cap_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_list_entries("ram", NULL, NULL, 1, &count));
}

void test_bb_storage_list_entries_zero_cap_null_out_is_ok_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    size_t count = 0;
    // ram leaves list_entries NULL, so this still resolves to UNSUPPORTED --
    // this test only proves cap==0 with out==NULL clears the INVALID_ARG
    // guard (size-probe shape), not a successful listing.
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_storage_list_entries("ram", NULL, NULL, 0, &count));
}

void test_bb_storage_list_entries_unknown_backend_returns_not_found(void)
{
    reset_all();
    bb_storage_ram_register();

    bb_storage_entry_t out[1];
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_list_entries("does_not_exist", NULL, out, 1, &count));
}

void test_bb_storage_list_entries_ram_backend_returns_unsupported(void)
{
    reset_all();
    bb_storage_ram_register();

    bb_storage_entry_t out[1];
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_storage_list_entries("ram", NULL, out, 1, &count));
}

void test_bb_storage_get_stats_null_backend_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    bb_storage_stats_t stats;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_get_stats(NULL, &stats));
}

void test_bb_storage_get_stats_null_out_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_get_stats("ram", NULL));
}

void test_bb_storage_get_stats_unknown_backend_returns_not_found(void)
{
    reset_all();
    bb_storage_ram_register();

    bb_storage_stats_t stats;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_get_stats("does_not_exist", &stats));
}

void test_bb_storage_get_stats_ram_backend_returns_unsupported(void)
{
    reset_all();
    bb_storage_ram_register();

    bb_storage_stats_t stats;
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_storage_get_stats("ram", &stats));
}

void test_bb_storage_register_backend_both_enum_set_succeeds(void)
{
    reset_all();
    register_fake_enum_backend();
}

void test_bb_storage_list_entries_dispatches_entries_through_from_backend(void)
{
    reset_all();
    register_fake_enum_backend();

    bb_storage_entry_t out[8] = {0};
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_list_entries("fake_enum", "wifi", out, 8, &count));
    TEST_ASSERT_EQUAL(3, count);
    TEST_ASSERT_EQUAL_STRING("wifi", out[0].ns_or_dir);
    TEST_ASSERT_EQUAL_STRING("ssid", out[0].key);
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_STR, out[0].enc);
    TEST_ASSERT_EQUAL(4, out[0].len);
    TEST_ASSERT_EQUAL_STRING("mode", out[2].key);
}

void test_bb_storage_list_entries_null_ns_or_dir_passes_through_as_all_namespaces(void)
{
    reset_all();
    register_fake_enum_backend();

    bb_storage_entry_t out[8] = {0};
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_list_entries("fake_enum", NULL, out, 8, &count));
    TEST_ASSERT_TRUE(s_last_list_ns_or_dir_was_null);
}

void test_bb_storage_list_entries_truncation_reports_true_count_over_cap(void)
{
    reset_all();
    register_fake_enum_backend();

    bb_storage_entry_t out[1] = {0};
    size_t count = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_list_entries("fake_enum", NULL, out, 1, &count));
    TEST_ASSERT_EQUAL(3, count);
    TEST_ASSERT_EQUAL_STRING("ssid", out[0].key);
}

void test_bb_storage_get_stats_dispatches_stats_through_from_backend(void)
{
    reset_all();
    register_fake_enum_backend();

    bb_storage_stats_t stats = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get_stats("fake_enum", &stats));
    TEST_ASSERT_EQUAL(128, stats.used_bytes);
    TEST_ASSERT_EQUAL(896, stats.free_bytes);
    TEST_ASSERT_EQUAL(1024, stats.total_bytes);
    TEST_ASSERT_EQUAL(2, stats.namespace_count);
}

/* ---------------------------------------------------------------------------
 * bb_storage_ram-specific overflow/validation branches
 * ---------------------------------------------------------------------------*/
void test_bb_storage_ram_set_table_full_returns_no_space(void)
{
    reset_all();
    bb_storage_ram_register();

    char keybuf[BB_STORAGE_RAM_MAX_ENTRIES + 1][16];
    for (int i = 0; i < BB_STORAGE_RAM_MAX_ENTRIES; i++) {
        snprintf(keybuf[i], sizeof(keybuf[i]), "k%d", i);
        bb_storage_addr_t addr = { .backend = "ram", .ns_or_dir = NULL, .key = keybuf[i] };
        TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&addr, "v", 1));
    }

    snprintf(keybuf[BB_STORAGE_RAM_MAX_ENTRIES], sizeof(keybuf[BB_STORAGE_RAM_MAX_ENTRIES]), "overflow");
    bb_storage_addr_t overflow_addr = { .backend = "ram", .ns_or_dir = NULL, .key = keybuf[BB_STORAGE_RAM_MAX_ENTRIES] };
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_storage_set(&overflow_addr, "v", 1));
}

void test_bb_storage_ram_set_value_too_large_returns_no_space(void)
{
    reset_all();
    bb_storage_ram_register();

    static uint8_t big[BB_STORAGE_RAM_MAX_VALUE_BYTES + 1];
    memset(big, 'a', sizeof(big));
    TEST_ASSERT_EQUAL(BB_ERR_NO_SPACE, bb_storage_set(&ADDR_FOO, big, sizeof(big)));
}

void test_bb_storage_ram_set_key_too_long_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    static char long_key[BB_STORAGE_RAM_MAX_KEY_BYTES + 1];
    memset(long_key, 'k', sizeof(long_key) - 1);
    long_key[sizeof(long_key) - 1] = '\0';
    bb_storage_addr_t addr = { .backend = "ram", .ns_or_dir = NULL, .key = long_key };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_set(&addr, "v", 1));
}

/* ---------------------------------------------------------------------------
 * bb_storage_ram_register duplicate call (bb_storage's dup policy applies)
 * ---------------------------------------------------------------------------*/
void test_bb_storage_ram_register_twice_returns_invalid_state(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_ram_register());
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_storage_ram_register());
}
