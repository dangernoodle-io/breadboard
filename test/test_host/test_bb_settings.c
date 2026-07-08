#include "unity.h"
#include "bb_settings.h"
#include "bb_wifi_creds.h"
#include "bb_storage.h"
#include "bb_config.h"

#include <string.h>

// bb_settings' default wifi-creds provider forwards to bb_config field
// accessors targeting backend="nvs" (the same ns/keys bb_nv_config uses --
// see bb_settings.c). The real "nvs" bb_storage backend is ESP-IDF-only, so
// these host tests register a small fake in-memory vtable under the name
// "nvs" -- exercising the SAME production field table/addr/provider code
// path bb_settings.c uses, with only the backend's storage swapped for a
// host-safe stand-in (mirrors test_bb_storage_typed.c's fake_get/fake_set
// pattern).
//
// The provider vtable itself has no setter (get_ssid/get_pass/has_creds/
// clear only -- see bb_wifi_creds.h) -- seeding test creds goes through a
// TEST-LOCAL bb_config_field_t pointing at the exact same addr (backend/ns/
// key) as bb_settings.c's internal fields, so a seeded value is visible
// through the real provider.

#define FAKE_NVS_MAX_ENTRIES 4
#define FAKE_NVS_MAX_VALUE   128
#define FAKE_NVS_KEY_MAX     32

typedef struct {
    bool    used;
    char    key[FAKE_NVS_KEY_MAX];
    size_t  len;
    uint8_t value[FAKE_NVS_MAX_VALUE];
} fake_nvs_entry_t;

static fake_nvs_entry_t s_fake_nvs[FAKE_NVS_MAX_ENTRIES];

// Fake-erase-fail hook: when set, fake_nvs_erase() returns s_fail_erase_code
// instead of BB_OK for the named key -- lets a test exercise settings_clear's
// ssid-erase-fails branch without a real NVS backend.
static const char *s_fail_erase_key  = NULL;
static bb_err_t    s_fail_erase_code = BB_OK;

static void fake_nvs_reset(void)
{
    memset(s_fake_nvs, 0, sizeof(s_fake_nvs));
    s_fail_erase_key  = NULL;
    s_fail_erase_code = BB_OK;
}

static void fake_nvs_set_erase_fail(const char *key, bb_err_t code)
{
    s_fail_erase_key  = key;
    s_fail_erase_code = code;
}

static fake_nvs_entry_t *fake_nvs_find(const char *key)
{
    if (key == NULL) return NULL;
    for (int i = 0; i < FAKE_NVS_MAX_ENTRIES; i++) {
        if (s_fake_nvs[i].used && strcmp(s_fake_nvs[i].key, key) == 0) {
            return &s_fake_nvs[i];
        }
    }
    return NULL;
}

static bb_err_t fake_nvs_get(void *impl, const bb_storage_addr_t *addr, void *buf, size_t cap, size_t *out_len)
{
    (void)impl;
    fake_nvs_entry_t *e = fake_nvs_find(addr->key);
    if (e == NULL) return BB_ERR_NOT_FOUND;
    *out_len = e->len;
    if (cap > 0) {
        size_t copy_len = e->len < cap ? e->len : cap;
        memcpy(buf, e->value, copy_len);
    }
    return BB_OK;
}

static bb_err_t fake_nvs_set(void *impl, const bb_storage_addr_t *addr, const void *buf, size_t len)
{
    (void)impl;
    if (len > FAKE_NVS_MAX_VALUE) return BB_ERR_NO_SPACE;

    fake_nvs_entry_t *e = fake_nvs_find(addr->key);
    if (e == NULL) {
        for (int i = 0; i < FAKE_NVS_MAX_ENTRIES; i++) {
            if (!s_fake_nvs[i].used) { e = &s_fake_nvs[i]; break; }
        }
        if (e == NULL) return BB_ERR_NO_SPACE;
        strncpy(e->key, addr->key, sizeof(e->key) - 1);
        e->key[sizeof(e->key) - 1] = '\0';
        e->used = true;
    }
    if (len > 0) memcpy(e->value, buf, len);
    e->len = len;
    return BB_OK;
}

static bb_err_t fake_nvs_erase(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl;
    if (s_fail_erase_key != NULL && addr->key != NULL && strcmp(addr->key, s_fail_erase_key) == 0) {
        return s_fail_erase_code;
    }
    fake_nvs_entry_t *e = fake_nvs_find(addr->key);
    if (e != NULL) memset(e, 0, sizeof(*e));
    return BB_OK;
}

static bool fake_nvs_exists(void *impl, const bb_storage_addr_t *addr)
{
    (void)impl;
    return fake_nvs_find(addr->key) != NULL;
}

static const bb_storage_vtable_t s_fake_nvs_vtable = {
    .get    = fake_nvs_get,
    .set    = fake_nvs_set,
    .erase  = fake_nvs_erase,
    .exists = fake_nvs_exists,
};

// Test-local field descriptors targeting the exact same addr (backend/ns/
// key) as bb_settings.c's internal s_wifi_ssid_field/s_wifi_pass_field, used
// only to seed/inspect state the real provider reads/clears.
static const bb_config_field_t s_test_ssid_field = {
    .id      = "wifi.ssid",
    .type    = BB_CONFIG_STR,
    .addr    = { .backend = "nvs", .ns_or_dir = "bb_cfg", .key = "wifi_ssid" },
    .max_len = 32,
};

static const bb_config_field_t s_test_pass_field = {
    .id      = "wifi.pass",
    .type    = BB_CONFIG_STR,
    .addr    = { .backend = "nvs", .ns_or_dir = "bb_cfg", .key = "wifi_pass" },
    .max_len = 64,
};

static void reset_all(void)
{
    bb_storage_test_reset();
    fake_nvs_reset();
    bb_storage_register_backend("nvs", &s_fake_nvs_vtable, NULL);
}

/* ---------------------------------------------------------------------------
 * get_ssid / get_pass round trip
 * ---------------------------------------------------------------------------*/
void test_bb_settings_provider_get_ssid_get_pass_round_trip(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_field, "MyNetwork"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_pass_field, "hunter2"));

    const bb_wifi_creds_provider_t *p = bb_settings_wifi_creds_provider();
    void *ctx = bb_settings_wifi_creds_ctx();

    char ssid[40] = {0};
    size_t ssid_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, p->get_ssid(ctx, ssid, sizeof(ssid), &ssid_len));
    TEST_ASSERT_EQUAL(strlen("MyNetwork"), ssid_len);
    TEST_ASSERT_EQUAL_STRING_LEN("MyNetwork", ssid, ssid_len);

    char pass[70] = {0};
    size_t pass_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, p->get_pass(ctx, pass, sizeof(pass), &pass_len));
    TEST_ASSERT_EQUAL(strlen("hunter2"), pass_len);
    TEST_ASSERT_EQUAL_STRING_LEN("hunter2", pass, pass_len);
}

/* ---------------------------------------------------------------------------
 * has_creds
 * ---------------------------------------------------------------------------*/
void test_bb_settings_provider_has_creds_false_when_unset(void)
{
    reset_all();
    const bb_wifi_creds_provider_t *p = bb_settings_wifi_creds_provider();
    TEST_ASSERT_FALSE(p->has_creds(bb_settings_wifi_creds_ctx()));
}

void test_bb_settings_provider_has_creds_true_after_set(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_field, "MyNetwork"));

    const bb_wifi_creds_provider_t *p = bb_settings_wifi_creds_provider();
    TEST_ASSERT_TRUE(p->has_creds(bb_settings_wifi_creds_ctx()));
}

/* ---------------------------------------------------------------------------
 * clear
 * ---------------------------------------------------------------------------*/
void test_bb_settings_provider_clear_resets_has_creds_false(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_field, "MyNetwork"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_pass_field, "hunter2"));

    const bb_wifi_creds_provider_t *p = bb_settings_wifi_creds_provider();
    void *ctx = bb_settings_wifi_creds_ctx();

    TEST_ASSERT_TRUE(p->has_creds(ctx));
    TEST_ASSERT_EQUAL(BB_OK, p->clear(ctx));
    TEST_ASSERT_FALSE(p->has_creds(ctx));
    TEST_ASSERT_FALSE(bb_config_exists(&s_test_pass_field));
}

void test_bb_settings_provider_clear_is_idempotent(void)
{
    reset_all();
    const bb_wifi_creds_provider_t *p = bb_settings_wifi_creds_provider();
    void *ctx = bb_settings_wifi_creds_ctx();

    // Erasing an absent value is BB_OK per bb_config_erase's contract.
    TEST_ASSERT_EQUAL(BB_OK, p->clear(ctx));
    TEST_ASSERT_FALSE(p->has_creds(ctx));
}

// settings_clear erases ssid then pass and returns the ssid error if it
// failed, rather than letting a (successful) pass-erase mask it -- exercise
// the true branch of that ternary: ssid-erase fails, pass-erase succeeds,
// and clear() must still propagate the ssid failure.
void test_bb_settings_provider_clear_propagates_ssid_erase_error(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_field, "MyNetwork"));
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_pass_field, "hunter2"));

    fake_nvs_set_erase_fail("wifi_ssid", BB_ERR_TIMEOUT);

    const bb_wifi_creds_provider_t *p = bb_settings_wifi_creds_provider();
    void *ctx = bb_settings_wifi_creds_ctx();

    TEST_ASSERT_EQUAL(BB_ERR_TIMEOUT, p->clear(ctx));

    // pass-erase still ran and succeeded -- confirms the ssid error wasn't
    // returned merely because pass-erase also failed.
    TEST_ASSERT_FALSE(bb_config_exists(&s_test_pass_field));
}

/* ---------------------------------------------------------------------------
 * cap=0 size-probe + truncation contract (mirrors bb_config_get_str)
 * ---------------------------------------------------------------------------*/
void test_bb_settings_provider_get_ssid_cap_zero_probes_length(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_field, "MyNetwork"));

    const bb_wifi_creds_provider_t *p = bb_settings_wifi_creds_provider();
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, p->get_ssid(bb_settings_wifi_creds_ctx(), NULL, 0, &out_len));
    TEST_ASSERT_EQUAL(strlen("MyNetwork"), out_len);
}

void test_bb_settings_provider_get_pass_truncation_reports_full_len(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_pass_field, "hunter2222"));

    const bb_wifi_creds_provider_t *p = bb_settings_wifi_creds_provider();
    char buf[4] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, p->get_pass(bb_settings_wifi_creds_ctx(), buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(strlen("hunter2222"), out_len);
    TEST_ASSERT_TRUE(out_len > sizeof(buf));
    TEST_ASSERT_EQUAL_STRING_LEN("hunt", buf, sizeof(buf));
}

/* ---------------------------------------------------------------------------
 * call-through-the-vtable-pointer -- assign the provider pointer through an
 * indirection layer (mimics a future bb_wifi consumer holding only the
 * abstract bb_wifi_creds_provider_t* + ctx) and confirm dispatch works.
 * ---------------------------------------------------------------------------*/
static bb_err_t call_get_ssid_via_indirection(const bb_wifi_creds_provider_t *p, void *ctx,
                                               char *buf, size_t cap, size_t *out_len)
{
    return p->get_ssid(ctx, buf, cap, out_len);
}

void test_bb_settings_provider_vtable_pointer_indirection(void)
{
    reset_all();
    TEST_ASSERT_EQUAL(BB_OK, bb_config_set_str(&s_test_ssid_field, "IndirectNet"));

    const bb_wifi_creds_provider_t *p = bb_settings_wifi_creds_provider();
    void *ctx = bb_settings_wifi_creds_ctx();

    char buf[32] = {0};
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(BB_OK, call_get_ssid_via_indirection(p, ctx, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL(strlen("IndirectNet"), out_len);
    TEST_ASSERT_EQUAL_STRING_LEN("IndirectNet", buf, out_len);
}

void test_bb_settings_wifi_creds_ctx_returns_null(void)
{
    // The default provider is stateless -- ctx is always NULL today.
    TEST_ASSERT_NULL(bb_settings_wifi_creds_ctx());
}
