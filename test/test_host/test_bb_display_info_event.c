// Host unit tests for bb_display's health.display cache/SSE surface:
// - bb_display_serialize (bb_cache serializer, replaces old pure builder)
// - bb_display_register_info (B1-893: re-homed from the deleted
//   bb_display_info satellite; host stub, no event bus)
#include "unity.h"
#include "bb_cache.h"
#include "bb_display_info.h"
#include "bb_display_info_event_priv.h"
#include "bb_json.h"

#include <string.h>
#include <stdlib.h>

// Test reset hook
void bb_cache_reset_for_test(void);

static void reset(void)
{
    bb_cache_reset_for_test();
}

// Helper: register topic, seed snap, serialize to JSON string (caller frees).
static char *serialize_snap(const bb_display_snap_t *snap)
{
    reset();
    bb_cache_config_t cfg = {
        .key       = BB_DISPLAY_INFO_TOPIC,
        .snapshot  = NULL,
        .snap_size = sizeof(bb_display_snap_t),
        .serialize = bb_display_serialize,
        .flags     = BB_CACHE_FLAG_NONE,
    };
    bb_cache_register(&cfg);
    bb_cache_update(&(bb_cache_update_t){ .key = BB_DISPLAY_INFO_TOPIC, .snap = snap });
    bb_json_t obj = bb_json_obj_new();
    bb_cache_serialize_into(BB_DISPLAY_INFO_TOPIC, obj);
    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    return json;
}

// ---------------------------------------------------------------------------
// present=true emits all 5 fields
// ---------------------------------------------------------------------------

void test_bb_display_serialize_present_true_all_fields(void)
{
    bb_display_snap_t snap = {
        .present = true,
        .panel   = "ek79007",
        .width   = 1024,
        .height  = 600,
        .enabled = true,
    };
    char *json = serialize_snap(&snap);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"present\":true"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"panel\":\"ek79007\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"width\":1024"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"height\":600"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"enabled\":true"));
    bb_json_free_str(json);
}

// ---------------------------------------------------------------------------
// present=false emits only present:false, no panel/width/height/enabled
// ---------------------------------------------------------------------------

void test_bb_display_serialize_present_false_only(void)
{
    bb_display_snap_t snap = {
        .present = false,
        .panel   = "",
        .width   = 0,
        .height  = 0,
        .enabled = false,
    };
    char *json = serialize_snap(&snap);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"present\":false"));
    TEST_ASSERT_NULL(strstr(json, "\"panel\""));
    TEST_ASSERT_NULL(strstr(json, "\"width\""));
    TEST_ASSERT_NULL(strstr(json, "\"height\""));
    TEST_ASSERT_NULL(strstr(json, "\"enabled\""));
    bb_json_free_str(json);
}

// ---------------------------------------------------------------------------
// panel string appears correctly for another panel name
// ---------------------------------------------------------------------------

void test_bb_display_serialize_panel_ssd1306(void)
{
    bb_display_snap_t snap = {
        .present = true,
        .panel   = "ssd1306",
        .width   = 128,
        .height  = 64,
        .enabled = true,
    };
    char *json = serialize_snap(&snap);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"panel\":\"ssd1306\""));
    bb_json_free_str(json);
}

// ---------------------------------------------------------------------------
// width/height appear correctly
// ---------------------------------------------------------------------------

void test_bb_display_serialize_width_height(void)
{
    bb_display_snap_t snap = {
        .present = true,
        .panel   = "mock",
        .width   = 320,
        .height  = 240,
        .enabled = false,
    };
    char *json = serialize_snap(&snap);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"width\":320"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"height\":240"));
    bb_json_free_str(json);
}

// ---------------------------------------------------------------------------
// enabled=false reflects correctly
// ---------------------------------------------------------------------------

void test_bb_display_serialize_enabled_false(void)
{
    bb_display_snap_t snap = {
        .present = true,
        .panel   = "mock",
        .width   = 320,
        .height  = 240,
        .enabled = false,
    };
    char *json = serialize_snap(&snap);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"enabled\":false"));
    bb_json_free_str(json);
}

// ---------------------------------------------------------------------------
// bb_display_register_info (host stub): registers the health.display cache
// key; a subsequent update+serialize round-trip proves registration
// actually succeeded (bb_cache_serialize_into fails on an unregistered key).
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

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_EQUAL(BB_OK, bb_cache_serialize_into(BB_DISPLAY_INFO_TOPIC, obj));
    char *json = bb_json_serialize(obj);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"panel\":\"mock\""));
    bb_json_free_str(json);
    bb_json_free(obj);
}
