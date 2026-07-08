#include "unity.h"
#include "bb_wifi_creds.h"

#include <string.h>

// bb_wifi_creds_read() is the pure provider-vs-fallback dispatcher pulled out
// of bb_wifi.c's wifi_read_ssid/wifi_read_pass (bb_wifi.c itself is
// ESP-IDF-only and not host-testable) -- these tests are the ones that would
// have caught the out_len==NULL regression: the provider path is asserted on
// buf CONTENTS, not just the return code.

typedef struct {
    const char *value;
    int calls;
    size_t last_cap;
    bool last_out_len_was_null; // true if called with out_len==NULL (would be a bug)
} fake_ctx_t;

// Mimics bb_settings's real provider shape: forwards to something that
// REJECTS out_len==NULL (bb_config_get_str's real contract) and, on a NULL
// out_len, writes NOTHING to buf and returns BB_ERR_INVALID_ARG -- this is
// exactly the shape that shipped broken when bb_wifi.c forwarded the
// caller's raw (NULL) out_len straight through.
static bb_err_t fake_provider_get(void *ctx, char *buf, size_t cap, size_t *out_len)
{
    fake_ctx_t *fc = (fake_ctx_t *)ctx;
    fc->calls++;
    fc->last_cap = cap;
    fc->last_out_len_was_null = (out_len == NULL);
    if (out_len == NULL) {
        return BB_ERR_INVALID_ARG; // real bb_config_get_str contract
    }
    size_t len = strlen(fc->value);
    *out_len = len;
    if (cap > 0) {
        size_t n = len < cap ? len : cap;
        memcpy(buf, fc->value, n);
        if (n < cap) buf[n] = '\0';
    }
    return BB_OK;
}

static bb_err_t fake_fallback_get(void *ctx, char *buf, size_t cap, size_t *out_len)
{
    fake_ctx_t *fc = (fake_ctx_t *)ctx;
    fc->calls++;
    fc->last_cap = cap;
    fc->last_out_len_was_null = (out_len == NULL);
    size_t len = strlen(fc->value);
    if (out_len) *out_len = len;
    if (cap > 0) {
        size_t n = len < cap ? len : cap;
        memcpy(buf, fc->value, n);
        if (n < cap) buf[n] = '\0';
    }
    return BB_OK;
}

// Provider path, caller's out_len non-NULL: buf is filled AND out_len
// reports the real length -- basic round trip.
void test_bb_wifi_creds_read_provider_round_trip(void)
{
    fake_ctx_t pctx = { .value = "MyNetwork" };
    char buf[40] = {0};
    size_t out_len = 999;

    bb_err_t err = bb_wifi_creds_read(fake_provider_get, &pctx, NULL, NULL, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(1, pctx.calls);
    TEST_ASSERT_FALSE(pctx.last_out_len_was_null);
    TEST_ASSERT_EQUAL(strlen("MyNetwork"), out_len);
    TEST_ASSERT_EQUAL_STRING("MyNetwork", buf);
}

// This is the regression test: caller passes out_len==NULL (the real
// bb_wifi.c connect-path call shape) with a provider set. Before the fix,
// bb_wifi.c forwarded the NULL straight to the provider -- which, per the
// real bb_config_get_str contract, rejects NULL and writes nothing to buf.
// bb_wifi_creds_read must ALWAYS pass a non-NULL out_len to the provider,
// so buf is actually populated even though the caller didn't ask for the
// length back.
void test_bb_wifi_creds_read_provider_caller_out_len_null_still_fills_buf(void)
{
    fake_ctx_t pctx = { .value = "MyNetwork" };
    char buf[40];
    memset(buf, 0xAA, sizeof(buf)); // poison

    bb_err_t err = bb_wifi_creds_read(fake_provider_get, &pctx, NULL, NULL, buf, sizeof(buf), NULL);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(1, pctx.calls);
    // The provider itself must never see a NULL out_len.
    TEST_ASSERT_FALSE(pctx.last_out_len_was_null);
    TEST_ASSERT_EQUAL_STRING("MyNetwork", buf);
}

// Fallback path (provider_fn == NULL): dispatches to fallback_fn, same
// non-NULL out_len guarantee, same buf-contents assertion.
void test_bb_wifi_creds_read_fallback_path(void)
{
    fake_ctx_t fctx = { .value = "FallbackNet" };
    char buf[40] = {0};
    size_t out_len = 0;

    bb_err_t err = bb_wifi_creds_read(NULL, NULL, fake_fallback_get, &fctx, buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(1, fctx.calls);
    TEST_ASSERT_EQUAL(strlen("FallbackNet"), out_len);
    TEST_ASSERT_EQUAL_STRING("FallbackNet", buf);
}

// Fallback path also tolerates the caller's out_len==NULL without crashing
// and without passing NULL down.
void test_bb_wifi_creds_read_fallback_caller_out_len_null(void)
{
    fake_ctx_t fctx = { .value = "FallbackNet" };
    char buf[40] = {0};

    bb_err_t err = bb_wifi_creds_read(NULL, NULL, fake_fallback_get, &fctx, buf, sizeof(buf), NULL);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_FALSE(fctx.last_out_len_was_null);
    TEST_ASSERT_EQUAL_STRING("FallbackNet", buf);
}

// Provider set takes precedence over fallback -- fallback must not be called.
void test_bb_wifi_creds_read_provider_takes_precedence_over_fallback(void)
{
    fake_ctx_t pctx = { .value = "ProviderNet" };
    fake_ctx_t fctx = { .value = "ShouldNotBeUsed" };
    char buf[40] = {0};
    size_t out_len = 0;

    bb_err_t err = bb_wifi_creds_read(fake_provider_get, &pctx, fake_fallback_get, &fctx,
                                       buf, sizeof(buf), &out_len);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(1, pctx.calls);
    TEST_ASSERT_EQUAL(0, fctx.calls);
    TEST_ASSERT_EQUAL_STRING("ProviderNet", buf);
}

// cap=0 size-probe: mirrors bb_config_get_str's contract -- length reported,
// buf untouched.
void test_bb_wifi_creds_read_cap_zero_probes_length(void)
{
    fake_ctx_t pctx = { .value = "ProbeNet" };
    size_t out_len = 0;

    bb_err_t err = bb_wifi_creds_read(fake_provider_get, &pctx, NULL, NULL, NULL, 0, &out_len);

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(strlen("ProbeNet"), out_len);
}
