// Host tests for bb_health_compose_and_stream() -- the gather-then-stream
// composer for /api/health (B1-1100, PR-5 of 6, epic B1-1054). Portable
// seam, host-testable per embedded.md ("factor decode/classify/compute
// logic into pure functions ... keep the platform call site a thin
// wrapper") -- the ESP-IDF handler (platform/espidf/bb_health/bb_health.c)
// only gathers the ROOT slice from bb_wifi/bb_mdns/bb_board and calls this.

#include "unity.h"

#include "../../components/bb_health/bb_health_compose_priv.h"
#include "../../components/bb_health/bb_health_section_priv.h"

#include "bb_http_host.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

typedef struct {
    int64_t n;
} probe_snap_t;

static const bb_serialize_field_t s_probe_fields[] = {
    { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(probe_snap_t, n) },
};

static const bb_serialize_desc_t s_probe_desc = {
    .type_name = "probe_snap_t",
    .fields    = s_probe_fields,
    .n_fields  = 1,
    .snap_size = sizeof(probe_snap_t),
};

static bb_err_t probe_fill_ok(void *dst, const bb_health_fill_args_t *args)
{
    ((probe_snap_t *)dst)->n = *(int64_t *)args->ctx;
    return BB_OK;
}

static bb_err_t probe_fill_fail(void *dst, const bb_health_fill_args_t *args)
{
    (void)dst;
    (void)args;
    return BB_ERR_INVALID_STATE;
}

static void hc_reset(void)
{
    bb_health_section_test_reset();
}

static bb_http_request_t *s_req;
static bb_http_host_capture_t s_cap;

static void cap_begin(void) { bb_http_host_capture_begin(&s_req); }
static void cap_end(void)   { bb_http_host_capture_end(s_req, &s_cap); }
static void cap_free(void)  { bb_http_host_capture_free(&s_cap); }

static const bb_health_wire_t k_sample_root = {
    .ok = true,
    .validated = true,
    .network = {
        .ssid = "testnet",
        .bssid = "aa:bb:cc:dd:ee:ff",
        .ip = "192.168.1.50",
        .connected = true,
        .mdns = { .ptr = NULL, .len = 0 },
    },
};

// ---------------------------------------------------------------------------
// 1. NULL argument validation
// ---------------------------------------------------------------------------

void test_bb_health_compose_null_req_returns_invalid_arg(void)
{
    hc_reset();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_health_compose_and_stream(NULL, &k_sample_root));
}

void test_bb_health_compose_null_root_returns_invalid_arg(void)
{
    hc_reset();
    cap_begin();
    bb_err_t err = bb_health_compose_and_stream(s_req, NULL);
    cap_end();
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
    cap_free();
}

// ---------------------------------------------------------------------------
// 2. Happy path -- root RAW group + registered sections as an OBJECT group,
// in that order.
// ---------------------------------------------------------------------------

void test_bb_health_compose_happy_path_no_sections(void)
{
    hc_reset();
    cap_begin();
    bb_err_t err = bb_health_compose_and_stream(s_req, &k_sample_root);
    cap_end();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING("application/json", s_cap.content_type);
    TEST_ASSERT_EQUAL_STRING(
        "{\"ok\":true,\"validated\":true,\"network\":{\"ssid\":\"testnet\","
        "\"bssid\":\"aa:bb:cc:dd:ee:ff\",\"ip\":\"192.168.1.50\","
        "\"connected\":true,\"mdns\":null}}",
        s_cap.body);
    cap_free();
}

void test_bb_health_compose_happy_path_with_sections(void)
{
    hc_reset();
    int64_t val_a = 1, val_b = 2;
    bb_health_section_t a = { .name = "alpha", .snap_desc = &s_probe_desc, .fill = probe_fill_ok, .ctx = &val_a };
    bb_health_section_t b = { .name = "beta",  .snap_desc = &s_probe_desc, .fill = probe_fill_ok, .ctx = &val_b };
    TEST_ASSERT_EQUAL(BB_OK, bb_health_section_register(&a));
    TEST_ASSERT_EQUAL(BB_OK, bb_health_section_register(&b));

    cap_begin();
    bb_err_t err = bb_health_compose_and_stream(s_req, &k_sample_root);
    cap_end();

    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_STRING(
        "{\"ok\":true,\"validated\":true,\"network\":{\"ssid\":\"testnet\","
        "\"bssid\":\"aa:bb:cc:dd:ee:ff\",\"ip\":\"192.168.1.50\","
        "\"connected\":true,\"mdns\":null},\"alpha\":{\"n\":1},\"beta\":{\"n\":2}}",
        s_cap.body);
    cap_free();
}

// ---------------------------------------------------------------------------
// 3. REVERT-DETECTOR (B1-1100 tripwire 1): a section whose fill fails must
// produce a clean 500 with NONE of the normal document's bytes -- proving
// gather-then-stream, not an interleaved gather/emit. A passing section
// registered BEFORE the failing one must not leak into the response
// either, since nothing streams until every fill has succeeded.
// ---------------------------------------------------------------------------

void test_bb_health_compose_section_fill_failure_returns_500_zero_normal_bytes(void)
{
    hc_reset();
    int64_t val_a = 1;
    bb_health_section_t passing = { .name = "alpha", .snap_desc = &s_probe_desc, .fill = probe_fill_ok, .ctx = &val_a };
    bb_health_section_t failing = { .name = "beta",  .snap_desc = &s_probe_desc, .fill = probe_fill_fail };
    TEST_ASSERT_EQUAL(BB_OK, bb_health_section_register(&passing));
    TEST_ASSERT_EQUAL(BB_OK, bb_health_section_register(&failing));

    cap_begin();
    bb_err_t err = bb_health_compose_and_stream(s_req, &k_sample_root);
    cap_end();

    // bb_http_send_json_error() returns bb_http_resp_sendstr()'s result --
    // BB_OK means the 500 itself was sent successfully (same convention as
    // bb_diag_section_dispatch.c's own fill-failure path); the tripwire is
    // the STATUS CODE and body contents, not this return code.
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL_INT(500, s_cap.status);
    // None of the normal document's bytes (root fields or the "alpha"
    // section registered before the failing "beta") ever hit the wire.
    TEST_ASSERT_NULL(strstr(s_cap.body ? s_cap.body : "", "\"ok\""));
    TEST_ASSERT_NULL(strstr(s_cap.body ? s_cap.body : "", "\"alpha\""));
    cap_free();
}

// See B1-1100's Phase-1/Phase-2 doc comment (bb_health_compose_priv.h) --
// this test's own comment records the manual revert exercise the reviewer
// asked for: temporarily interleaving each section's fill+stream (rather
// than gathering all sections first) reproduces a RED failure on the
// assertion above, because the passing "alpha" section's bytes are already
// flushed to the capture buffer by the time "beta" fails -- restoring the
// phase split turns it GREEN again. See the PR report for the captured
// RED/GREEN transcript.

// ---------------------------------------------------------------------------
// 4. Table-full boundary -- exactly BB_HEALTH_SECTION_TABLE_CAP sections all
// compose correctly (arena/entries sizing exercised at the real cap).
// ---------------------------------------------------------------------------

void test_bb_health_compose_table_full_all_sections_render(void)
{
    hc_reset();
    static int64_t vals[BB_HEALTH_SECTION_TABLE_CAP];
    static char names[BB_HEALTH_SECTION_TABLE_CAP][BB_HEALTH_SECTION_NAME_MAX];
    for (int i = 0; i < BB_HEALTH_SECTION_TABLE_CAP; i++) {
        vals[i] = i;
        snprintf(names[i], sizeof(names[i]), "s%d", i);
        bb_health_section_t sec = { .name = names[i], .snap_desc = &s_probe_desc, .fill = probe_fill_ok, .ctx = &vals[i] };
        TEST_ASSERT_EQUAL(BB_OK, bb_health_section_register(&sec));
    }

    cap_begin();
    bb_err_t err = bb_health_compose_and_stream(s_req, &k_sample_root);
    cap_end();

    TEST_ASSERT_EQUAL(BB_OK, err);
    for (int i = 0; i < BB_HEALTH_SECTION_TABLE_CAP; i++) {
        TEST_ASSERT_NOT_NULL(strstr(s_cap.body, names[i]));
    }
    cap_free();
}
