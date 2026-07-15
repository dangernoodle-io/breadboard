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
