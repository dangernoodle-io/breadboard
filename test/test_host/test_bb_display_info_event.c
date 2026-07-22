// Host unit tests for bb_display_register_info (B1-893: re-homed from the
// deleted bb_display_info satellite; host stub, no event bus).
//
// B1-1146a CUTOVER NOTE: like diag.boot's (B1-1053 PR1) and update.available's
// (B1-1053 PR3) own conversions, the legacy bb_json bb_cache serializer this
// file used to exercise (bb_display_serialize()) is DELETED -- there is no
// more "old" implementation to round-trip through bb_cache_serialize_into().
// The present/false, panel-name, width/height, and enabled-flag field
// coverage that used to live here now lives in test_wire_desc_producers.c's
// literal-golden bb_display_info_wire_desc tests (4b section) instead --
// this file's remaining job is proving bb_display_register_info() (host
// stub) actually wires the cache entry AND the bb_data bind.
#include "unity.h"
#include "bb_cache.h"
#include "bb_data.h"
#include "bb_display_info.h"
#include "bb_display_info_event_priv.h"
#include "bb_display_info_wire.h"
#include "bb_json.h"
#include "bb_serialize.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Test reset hooks (BB_CACHE_TESTING / BB_DATA_TESTING).
void bb_cache_reset_for_test(void);

static void reset(void)
{
    bb_cache_reset_for_test();
    bb_data_test_reset();
}

// ---------------------------------------------------------------------------
// bb_display_register_info (host stub) registers the health.display cache
// key: a subsequent update+get_raw round trip proves registration actually
// succeeded (bb_cache_get_raw fails on an unregistered key).
// ---------------------------------------------------------------------------

void test_bb_display_register_info_registers_cache_key(void)
{
    reset();
    bb_display_register_info();

    bb_display_snap_t snap = {
        .present = true,
        .panel   = "mock",
        .width   = 320,
        .height  = 240,
        .enabled = true,
    };
    TEST_ASSERT_EQUAL(BB_OK,
        bb_cache_update(&(bb_cache_update_t){ .key = BB_DISPLAY_INFO_TOPIC, .snap = &snap }));

    bb_display_snap_t out = {0};
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_get_raw(BB_DISPLAY_INFO_TOPIC, &out, sizeof(out)));
    TEST_ASSERT_TRUE(out.present);
    TEST_ASSERT_EQUAL_STRING("mock", out.panel);
}

// ---------------------------------------------------------------------------
// cfg->serialize is intentionally omitted at registration (B1-1146a) -- the
// legacy bb_json-embed path (bb_cache_serialize_into) is now BB_ERR_UNSUPPORTED
// for this key, matching diag.boot's and update.available's own post-cutover
// contract (see test_bb_cache_fidelity.c's register_null_serialize_accepted
// family). health.display's REST exposure is being rehomed to
// system.display under bb_system's diag endpoint (B1-1150), not this
// component -- this key now renders via bb_data for whoever binds it there.
// ---------------------------------------------------------------------------

void test_bb_display_register_info_serialize_into_returns_unsupported(void)
{
    reset();
    bb_display_register_info();

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_cache_serialize_into(BB_DISPLAY_INFO_TOPIC, obj));
    bb_json_free(obj);
}

// ---------------------------------------------------------------------------
// bb_display_register_info() self-binds "health.display" to bb_data
// (bb_display_info_bind(), bb_display_info_wire.c) -- a subsequent
// bb_data_render() round trip proves the bind actually took.
// ---------------------------------------------------------------------------

void test_bb_display_register_info_binds_bb_data(void)
{
    reset();
    bb_display_register_info();

    bb_display_snap_t snap = {
        .present = true,
        .panel   = "ili9341",
        .width   = 240,
        .height  = 320,
        .enabled = true,
    };
    TEST_ASSERT_EQUAL(BB_OK,
        bb_cache_update(&(bb_cache_update_t){ .key = BB_DISPLAY_INFO_TOPIC, .snap = &snap }));

    char   scratch[sizeof(bb_display_info_wire_t)];
    char   buf[256];
    size_t out_len = 0;
    bb_data_render_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = BB_DISPLAY_INFO_TOPIC, .query = NULL,
        .scratch = scratch, .scratch_cap = sizeof(scratch),
        .buf = buf, .buf_cap = sizeof(buf), .out_len = &out_len,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_data_render(&req));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"panel\":\"ili9341\""));
}

// ---------------------------------------------------------------------------
// bb_display_register_info()'s bb_cache_register() call site -- a registry-
// full failure (BB_CACHE_MAX_TOPICS already exhausted by other registered
// keys) is non-fatal: the function logs a warning and returns early, WITHOUT
// reaching bb_display_info_bind() below it. Fills the registry with dummy
// keys first (mirrors test_bb_cache_registry_full, test_bb_cache_fidelity.c),
// leaving no free slot for "health.display".
// ---------------------------------------------------------------------------

typedef struct { int64_t n; } dummy_cache_snap_t;

void test_bb_display_register_info_cache_register_failure_is_non_fatal(void)
{
    reset();

    char key_buf[BB_CACHE_MAX_TOPICS][32];
    for (int i = 0; i < BB_CACHE_MAX_TOPICS; i++) {
        snprintf(key_buf[i], sizeof(key_buf[i]), "test.fill.%d", i);
        bb_cache_config_t cfg = {
            .key       = key_buf[i],
            .snapshot  = NULL,
            .snap_size = sizeof(dummy_cache_snap_t),
        };
        TEST_ASSERT_EQUAL(BB_OK, bb_cache_register(&cfg));
    }

    bb_display_register_info();

    // bb_cache_register() failed (registry full) -- the key was never
    // registered, so a subsequent update/get_raw round trip must miss.
    bb_display_snap_t snap = { .present = true };
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND,
        bb_cache_update(&(bb_cache_update_t){ .key = BB_DISPLAY_INFO_TOPIC, .snap = &snap }));

    // bb_display_info_bind() was never reached either -- unbound.
    char   scratch[sizeof(bb_display_info_wire_t)];
    char   buf[256];
    size_t out_len = 0;
    bb_data_render_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = BB_DISPLAY_INFO_TOPIC, .query = NULL,
        .scratch = scratch, .scratch_cap = sizeof(scratch),
        .buf = buf, .buf_cap = sizeof(buf), .out_len = &out_len,
    };
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_data_render(&req));

    // Restore a clean cache registry: this test's BB_CACHE_MAX_TOPICS dummy
    // entries would otherwise starve the registry for every test that runs
    // afterward in this same process.
    bb_cache_reset_for_test();
}

// ---------------------------------------------------------------------------
// bb_display_register_info()'s own bb_display_info_bind() call site -- a
// bind failure (BB_DATA_MAX_BINDINGS already exhausted by other composed
// keys) is non-fatal (mirrors bb_diag_boot_bind()'s call site,
// bb_diag_routes.c, and bb_ota_check_bind()'s,
// test_ota_check_init_bind_failure_is_non_fatal_and_leaves_key_unbound,
// test_bb_ota_check_emit.c:543 -- same shape, mirrored here) but leaves
// "health.display" unbound, even though the bb_cache entry itself IS
// registered (bb_cache_register() runs, and succeeds, before the bind
// attempt).
// ---------------------------------------------------------------------------

typedef struct { int64_t n; } dummy_data_snap_t;

static const bb_serialize_field_t s_dummy_data_fields[] = {
    { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(dummy_data_snap_t, n) },
};

static const bb_serialize_desc_t s_dummy_data_desc = {
    .type_name = "dummy_data_snap_t",
    .fields    = s_dummy_data_fields,
    .n_fields  = 1,
    .snap_size = sizeof(dummy_data_snap_t),
};

static bb_err_t dummy_data_gather(void *dst, const bb_data_gather_args_t *args)
{
    (void)args;
    ((dummy_data_snap_t *)dst)->n = 0;
    return BB_OK;
}

void test_bb_display_register_info_bind_failure_is_non_fatal_and_leaves_key_unbound(void)
{
    reset();

    char keys[BB_DATA_MAX_BINDINGS][32];
    for (int i = 0; i < BB_DATA_MAX_BINDINGS; i++) {
        snprintf(keys[i], sizeof(keys[i]), "dummy.%d", i);
        bb_data_binding_t b = { .key = keys[i], .desc = &s_dummy_data_desc, .gather = dummy_data_gather };
        TEST_ASSERT_EQUAL(BB_OK, bb_data_bind(&b));
    }

    bb_display_register_info();

    // bb_cache_register() succeeded (registry has room) -- the cache entry
    // IS registered, distinguishing this from the registry-full test above.
    bb_display_snap_t snap = { .present = true };
    TEST_ASSERT_EQUAL(BB_OK,
        bb_cache_update(&(bb_cache_update_t){ .key = BB_DISPLAY_INFO_TOPIC, .snap = &snap }));

    // ...but bb_display_info_bind() failed (table full), so the key is
    // unbound: bb_data_render() propagates BB_ERR_NOT_FOUND.
    char   scratch[sizeof(bb_display_info_wire_t)];
    char   buf[256];
    size_t out_len = 0;
    bb_data_render_req_t req = {
        .fmt = BB_FORMAT_JSON, .key = BB_DISPLAY_INFO_TOPIC, .query = NULL,
        .scratch = scratch, .scratch_cap = sizeof(scratch),
        .buf = buf, .buf_cap = sizeof(buf), .out_len = &out_len,
    };
    TEST_ASSERT_EQUAL(BB_ERR_NOT_FOUND, bb_data_render(&req));

    // Restore a clean global bb_data table: this test's BB_DATA_MAX_BINDINGS
    // dummy bindings would otherwise starve it for every test that runs
    // afterward in this same process (bb_data's binding table is a single
    // global static, not reset automatically between tests).
    bb_data_test_reset();
}
