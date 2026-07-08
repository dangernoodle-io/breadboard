#include "unity.h"
#include "bb_storage.h"
#include "bb_storage_ram.h"
#include "bb_core.h"

#include <string.h>

// bb_storage_get_typed/set_typed facade: dispatch-to-backend-hook when a
// backend registers get_typed/set_typed, and fallback-to-plain-get/set (enc
// ignored, blob semantics) when it doesn't — see bb_storage.h's contract
// comment. Also covers bb_storage_register_backend's get_typed/set_typed
// pair validation (both NULL or both set).

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
 * Fallback path: a backend with no get_typed/set_typed (e.g. ram) behaves
 * exactly like the plain get/set facade — enc is ignored.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_get_typed_falls_back_to_get_on_ram_backend(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set(&ADDR_FOO, "hello", 5));

    char buf[16] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get_typed(&ADDR_FOO, BB_STORAGE_ENC_STR, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(5, out_len);
    TEST_ASSERT_EQUAL_STRING_LEN("hello", buf, out_len);
}

void test_bb_storage_set_typed_falls_back_to_set_on_ram_backend(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set_typed(&ADDR_FOO, BB_STORAGE_ENC_U32, "abcd", 4));

    char buf[8] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get(&ADDR_FOO, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(4, out_len);
    TEST_ASSERT_EQUAL_STRING_LEN("abcd", buf, out_len);
}

/* ---------------------------------------------------------------------------
 * Dispatch path: a test-only vtable registering get_typed/set_typed proves
 * the facade calls them (instead of falling back) when present, and passes
 * the enc value through unmodified.
 * ---------------------------------------------------------------------------*/
static bb_storage_enc_t s_last_get_enc;
static bb_storage_enc_t s_last_set_enc;
static int              s_get_typed_calls;
static int              s_set_typed_calls;

static bb_err_t fake_get(void *impl, const bb_storage_addr_t *addr, void *buf, size_t cap, size_t *out_len)
{
    (void)impl; (void)addr; (void)buf; (void)cap;
    *out_len = 0;
    return BB_OK;
}
static bb_err_t fake_set(void *impl, const bb_storage_addr_t *addr, const void *buf, size_t len)
{
    (void)impl; (void)addr; (void)buf; (void)len;
    return BB_OK;
}
static bb_err_t fake_erase(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl; (void)addr;
    return BB_OK;
}
static bool fake_exists(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl; (void)addr;
    return false;
}
static bb_err_t fake_get_typed(void *impl, const bb_storage_addr_t *addr, bb_storage_enc_t enc,
                                void *buf, size_t cap, size_t *out_len)
{
    (void)impl; (void)addr; (void)buf; (void)cap;
    s_get_typed_calls++;
    s_last_get_enc = enc;
    *out_len = 0;
    return BB_OK;
}
static bb_err_t fake_set_typed(void *impl, const bb_storage_addr_t *addr, bb_storage_enc_t enc,
                                const void *buf, size_t len)
{
    (void)impl; (void)addr; (void)buf; (void)len;
    s_set_typed_calls++;
    s_last_set_enc = enc;
    return BB_OK;
}

static const bb_storage_addr_t ADDR_FAKE = {
    .backend   = "fake_typed",
    .ns_or_dir = NULL,
    .key       = "k",
};

static void register_fake_typed_backend(void)
{
    s_last_get_enc = BB_STORAGE_ENC_BLOB;
    s_last_set_enc = BB_STORAGE_ENC_BLOB;
    s_get_typed_calls = 0;
    s_set_typed_calls = 0;
    bb_storage_vtable_t vt = {
        .get       = fake_get,
        .set       = fake_set,
        .erase     = fake_erase,
        .exists    = fake_exists,
        .get_typed = fake_get_typed,
        .set_typed = fake_set_typed,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_register_backend("fake_typed", &vt, NULL));
}

void test_bb_storage_get_typed_dispatches_to_backend_hook_when_present(void)
{
    reset_all();
    register_fake_typed_backend();

    char buf[4];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_get_typed(&ADDR_FAKE, BB_STORAGE_ENC_U16, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(1, s_get_typed_calls);
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_U16, s_last_get_enc);
}

void test_bb_storage_set_typed_dispatches_to_backend_hook_when_present(void)
{
    reset_all();
    register_fake_typed_backend();

    TEST_ASSERT_EQUAL(BB_OK, bb_storage_set_typed(&ADDR_FAKE, BB_STORAGE_ENC_I32, "abcd", 4));
    TEST_ASSERT_EQUAL(1, s_set_typed_calls);
    TEST_ASSERT_EQUAL(BB_STORAGE_ENC_I32, s_last_set_enc);
}

/* ---------------------------------------------------------------------------
 * Facade-level validation branches (mirror bb_storage_get/set's own).
 * ---------------------------------------------------------------------------*/
void test_bb_storage_get_typed_null_addr_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    char buf[4];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_get_typed(NULL, BB_STORAGE_ENC_BLOB, buf, sizeof(buf), &out_len));
}

void test_bb_storage_get_typed_null_backend_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    bb_storage_addr_t addr = { .backend = NULL, .ns_or_dir = NULL, .key = "foo" };
    char buf[4];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_get_typed(&addr, BB_STORAGE_ENC_BLOB, buf, sizeof(buf), &out_len));
}

void test_bb_storage_get_typed_unknown_backend_returns_not_found(void)
{
    reset_all();
    bb_storage_ram_register();

    char buf[4];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
                       bb_storage_get_typed(&ADDR_UNKNOWN_BACKEND, BB_STORAGE_ENC_BLOB, buf, sizeof(buf), &out_len));
}

void test_bb_storage_get_typed_null_out_len_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    char buf[4];
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_get_typed(&ADDR_FOO, BB_STORAGE_ENC_BLOB, buf, sizeof(buf), NULL));
}

void test_bb_storage_get_typed_null_buf_with_nonzero_cap_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_get_typed(&ADDR_FOO, BB_STORAGE_ENC_BLOB, NULL, 4, &out_len));
}

void test_bb_storage_set_typed_null_addr_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_set_typed(NULL, BB_STORAGE_ENC_BLOB, "x", 1));
}

void test_bb_storage_set_typed_unknown_backend_returns_not_found(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_storage_set_typed(&ADDR_UNKNOWN_BACKEND, BB_STORAGE_ENC_BLOB, "x", 1));
}

void test_bb_storage_set_typed_null_buf_with_nonzero_len_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_ram_register();

    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_set_typed(&ADDR_FOO, BB_STORAGE_ENC_BLOB, NULL, 4));
}

/* ---------------------------------------------------------------------------
 * bb_storage_register_backend: get_typed/set_typed pair validation.
 * ---------------------------------------------------------------------------*/
void test_bb_storage_register_backend_get_typed_without_set_typed_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_vtable_t vt = {
        .get = fake_get, .set = fake_set, .erase = fake_erase, .exists = fake_exists,
        .get_typed = fake_get_typed, .set_typed = NULL,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_register_backend("partial_typed", &vt, NULL));
}

void test_bb_storage_register_backend_set_typed_without_get_typed_returns_invalid_arg(void)
{
    reset_all();
    bb_storage_vtable_t vt = {
        .get = fake_get, .set = fake_set, .erase = fake_erase, .exists = fake_exists,
        .get_typed = NULL, .set_typed = fake_set_typed,
    };
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_storage_register_backend("partial_typed2", &vt, NULL));
}

void test_bb_storage_register_backend_both_typed_null_succeeds(void)
{
    reset_all();
    bb_storage_vtable_t vt = { .get = fake_get, .set = fake_set, .erase = fake_erase, .exists = fake_exists };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_register_backend("no_typed", &vt, NULL));
}

void test_bb_storage_register_backend_both_typed_set_succeeds(void)
{
    reset_all();
    bb_storage_vtable_t vt = {
        .get = fake_get, .set = fake_set, .erase = fake_erase, .exists = fake_exists,
        .get_typed = fake_get_typed, .set_typed = fake_set_typed,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_storage_register_backend("both_typed", &vt, NULL));
}
