// Host unit tests for bb_diag_boot_render_envelope() (B1-1053 PR1) -- the
// REST GET /api/diag/boot render path's portable seam: renders the
// "diag.boot" bb_data binding as {"ts_ms":N,"data":{...}} directly onto a
// bb_http_request_t. Exercised here via the host capture harness (mirrors
// test_bb_http_json_obj_stream.c) -- this is the SAME function
// boot_get_handler() (platform/espidf/bb_diag/bb_diag_routes.c) calls, no
// mirror/duplicate render path.
//
// ts_ms is bb_clock_now_ms64() read at render time (see
// bb_diag_boot_render_envelope()'s own doc, bb_diag_boot_wire.h, for the
// chosen semantic) -- not deterministic here (no bb_clock host test hook
// exists), so these tests assert the "ts_ms" prefix STRUCTURALLY (present,
// numeric, immediately followed by the "data" key) and the "data" portion
// EXACT-JSON.

#include "unity.h"

#include "bb_cache.h"
#include "bb_data.h"
#include "bb_diag_boot_wire.h"
#include "bb_diag_event_priv.h"
#include "bb_http.h"
#include "bb_http_host.h"

#include <string.h>

// Test reset hooks (BB_CACHE_TESTING / BB_DATA_TESTING).
void bb_cache_reset_for_test(void);

static void reset_world(void)
{
    bb_cache_reset_for_test();
    bb_data_test_reset();
}

// Registers + seeds the diag.boot bb_cache entry and binds it to bb_data --
// the same two steps bb_diag_routes_init() performs in production (see
// bb_diag_boot_wire.c's own bb_cache_register()/bb_diag_boot_bind() call
// sites), except cfg->serialize is left NULL here too (matching production:
// this key has no legacy bb_json serializer since B1-1053 PR1).
static void seed_and_bind_diag_boot(const bb_diag_boot_snap_t *snap)
{
    bb_cache_config_t cfg = {
        .key       = BB_DIAG_BOOT_TOPIC,
        .snapshot  = NULL,
        .snap_size = sizeof(bb_diag_boot_snap_t),
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_update(&(bb_cache_update_t){ .key = BB_DIAG_BOOT_TOPIC, .snap = snap }));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_diag_boot_bind());
}

// ---------------------------------------------------------------------------
// Envelope shape
// ---------------------------------------------------------------------------

void test_diag_boot_render_envelope_shape(void)
{
    reset_world();
    bb_diag_boot_snap_t snap = {
        .reset_reason      = "panic",
        .wdt_resets        = 3,
        .panic_available   = true,
        .panic_boots_since = 2,
    };
    seed_and_bind_diag_boot(&snap);

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_diag_boot_render_envelope(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL_INT(BB_OK, err);
    TEST_ASSERT_NOT_NULL(cap.body);

    // ts_ms: "{"ts_ms":" followed by one or more digits, then ",\"data\":".
    TEST_ASSERT_EQUAL_INT(0, strncmp(cap.body, "{\"ts_ms\":", 9));
    const char *p = cap.body + 9;
    TEST_ASSERT_TRUE(*p >= '0' && *p <= '9');
    while (*p >= '0' && *p <= '9') p++;
    TEST_ASSERT_EQUAL_INT(0, strncmp(p, ",\"data\":", 8));

    // "data": exact-JSON from here to the end of the response (byte-exact --
    // bb_data_render()'s output is deterministic for a fixed fixture).
    const char *data_start = strstr(cap.body, "\"data\":");
    TEST_ASSERT_NOT_NULL(data_start);
    TEST_ASSERT_EQUAL_STRING(
        "\"data\":{\"reset_reason\":\"panic\",\"wdt_resets\":3,"
        "\"panic\":{\"available\":true,\"boots_since\":2},"
        "\"pending_verify\":false,\"rolled_back\":false,"
        "\"reboot_reason\":{\"source\":\"unknown\",\"uptime_s\":0,\"epoch_s\":0},"
        "\"reboot_history\":[]}}",
        data_start);

    bb_http_host_capture_free(&cap);
}

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------

void test_diag_boot_render_envelope_null_req(void)
{
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_diag_boot_render_envelope(NULL));
}

// bb_data_render() resolves the "diag.boot" binding FIRST -- with no
// bb_diag_boot_bind() call, this propagates BB_ERR_NOT_FOUND straight
// through, regardless of bb_cache state.
void test_diag_boot_render_envelope_unbound_key_propagates_not_found(void)
{
    reset_world();

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_err_t err = bb_diag_boot_render_envelope(req);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, err);
    bb_http_host_capture_free(&cap);
}

// bb_data_render() succeeds (binding present + seeded), but the subsequent
// bb_http_resp_json_obj_begin() call fails (e.g. the response's Content-Type
// can't be set) -- exercises the `if (err != BB_OK) return err;` guard right
// after obj_begin() in bb_diag_boot_render_envelope(), distinct from the
// bb_data_render() failure path above.
void test_diag_boot_render_envelope_obj_begin_fails(void)
{
    reset_world();
    bb_diag_boot_snap_t snap = {
        .reset_reason      = "panic",
        .wdt_resets        = 0,
        .panic_available   = false,
        .panic_boots_since = 0,
    };
    seed_and_bind_diag_boot(&snap);

    bb_http_request_t *req;
    bb_http_host_capture_begin(&req);
    bb_http_host_force_set_type_fail(true);
    bb_err_t err = bb_diag_boot_render_envelope(req);
    bb_http_host_force_set_type_fail(false);
    bb_http_host_capture_t cap;
    bb_http_host_capture_end(req, &cap);

    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, err);
    bb_http_host_capture_free(&cap);
}
