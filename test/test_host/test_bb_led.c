// Tests for bb_led capability-aware dispatch using a mock driver.
#include "unity.h"
#include "bb_led.h"
#include "bb_led_driver.h"
#include <string.h>

typedef struct {
    bool     on[8];
    uint8_t  pct[8];
    uint16_t level[8];
    bool     level_seen[8];
    uint8_t  r[8], g[8], b[8];
    bool     flushed;
    bool     closed;
} mock_state_t;

static mock_state_t g_mock;

static bb_err_t m_set_on        (void *s, uint16_t i, bool v)                          { ((mock_state_t *)s)->on[i] = v; return BB_OK; }
static bb_err_t m_set_brightness(void *s, uint16_t i, uint8_t p)                       { ((mock_state_t *)s)->pct[i] = p; return BB_OK; }
static bb_err_t m_set_level     (void *s, uint16_t i, uint16_t l)                      { mock_state_t *m = s; m->level[i]=l; m->level_seen[i]=true; return BB_OK; }
static bb_err_t m_set_color     (void *s, uint16_t i, uint8_t r, uint8_t g, uint8_t b) { mock_state_t *m = s; m->r[i]=r; m->g[i]=g; m->b[i]=b; return BB_OK; }
static bb_err_t m_flush         (void *s)                                              { ((mock_state_t *)s)->flushed = true; return BB_OK; }
static bb_err_t m_close         (void *s)                                              { ((mock_state_t *)s)->closed  = true; return BB_OK; }

// No set_level → exercises the core's level→pct bridge.
static const bb_led_driver_t mock_full = {
    .set_on=m_set_on, .set_brightness=m_set_brightness, .set_color=m_set_color,
    .flush=m_flush, .close=m_close,
    .caps  = BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS | BB_LED_CAP_RGB,
    .count = 4,
};

static const bb_led_driver_t mock_onoff_only = {
    .set_on=m_set_on, .set_brightness=m_set_brightness, .set_color=m_set_color,
    .flush=m_flush, .close=m_close,
    .caps  = BB_LED_CAP_ONOFF,
    .count = 1,
};

// Has set_level → bb_led_set_level should call it directly (no bridge).
static const bb_led_driver_t mock_with_level = {
    .set_on=m_set_on, .set_brightness=m_set_brightness, .set_level=m_set_level,
    .set_color=m_set_color, .flush=m_flush, .close=m_close,
    .caps  = BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS,
    .count = 1,
};

// Reset mock state between tests — called by test_main.c setUp().
void bb_led_test_reset(void)
{
    memset(&g_mock, 0, sizeof g_mock);
    bb_led_set_primary(NULL);
}

void test_bb_led_caps_and_count(void)
{
    bb_led_handle_t h;
    TEST_ASSERT_EQUAL(BB_OK, bb_led_handle_create(&mock_full, &g_mock, &h));
    TEST_ASSERT_EQUAL(BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS | BB_LED_CAP_RGB, bb_led_caps(h));
    TEST_ASSERT_EQUAL_UINT16(4, bb_led_count(h));
    bb_led_close(h);
}

void test_bb_led_set_on(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_full, &g_mock, &h);
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_on(h, 2, true));
    TEST_ASSERT_TRUE(g_mock.on[2]);
    bb_led_close(h);
}

void test_bb_led_set_color_unsupported_when_no_rgb_cap(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_onoff_only, &g_mock, &h);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_led_set_color(h, 0, 1, 2, 3));
    TEST_ASSERT_EQUAL_UINT8(0, g_mock.r[0]); // driver not called
    bb_led_close(h);
}

void test_bb_led_idx_out_of_range(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_full, &g_mock, &h);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_set_on(h, 99, true));
    bb_led_close(h);
}

void test_bb_led_fill_color_iterates(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_full, &g_mock, &h);
    TEST_ASSERT_EQUAL(BB_OK, bb_led_fill_color(h, 0xAA, 0xBB, 0xCC));
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT8(0xAA, g_mock.r[i]);
        TEST_ASSERT_EQUAL_UINT8(0xBB, g_mock.g[i]);
        TEST_ASSERT_EQUAL_UINT8(0xCC, g_mock.b[i]);
    }
    bb_led_close(h);
}

void test_bb_led_close_calls_driver(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_full, &g_mock, &h);
    TEST_ASSERT_EQUAL(BB_OK, bb_led_close(h));
    TEST_ASSERT_TRUE(g_mock.closed);
}

void test_bb_led_brightness_pct_validation(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_full, &g_mock, &h);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_led_set_brightness(h, 0, 101));
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_brightness(h, 0, 100));
    TEST_ASSERT_EQUAL_UINT8(100, g_mock.pct[0]);
    bb_led_close(h);
}

void test_bb_led_set_level_bridges_to_set_brightness(void)
{
    // mock_full has no set_level → core maps level→pct (round to nearest).
    bb_led_handle_t h;
    bb_led_handle_create(&mock_full, &g_mock, &h);
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_level(h, 0, 65535));
    TEST_ASSERT_EQUAL_UINT8(100, g_mock.pct[0]);
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_level(h, 0, 0));
    TEST_ASSERT_EQUAL_UINT8(0, g_mock.pct[0]);
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_level(h, 0, 32768));  // ~50%
    TEST_ASSERT_EQUAL_UINT8(50, g_mock.pct[0]);
    TEST_ASSERT_FALSE(g_mock.level_seen[0]);                  // bridge, not direct
    bb_led_close(h);
}

void test_bb_led_set_level_calls_driver_when_present(void)
{
    // mock_with_level has set_level → called directly, set_brightness untouched.
    bb_led_handle_t h;
    bb_led_handle_create(&mock_with_level, &g_mock, &h);
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_level(h, 0, 40000));
    TEST_ASSERT_TRUE(g_mock.level_seen[0]);
    TEST_ASSERT_EQUAL_UINT16(40000, g_mock.level[0]);
    TEST_ASSERT_EQUAL_UINT8(0, g_mock.pct[0]);               // set_brightness not called
    bb_led_close(h);
}

void test_bb_led_set_level_unsupported_without_brightness_cap(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_onoff_only, &g_mock, &h);
    TEST_ASSERT_EQUAL(BB_ERR_UNSUPPORTED, bb_led_set_level(h, 0, 30000));
    bb_led_close(h);
}

/* ---------------------------------------------------------------------------
 * Tests: bb_led_primary / bb_led_set_primary / bb_led_name accessors
 * --------------------------------------------------------------------------- */

void test_bb_led_primary_null_before_set(void)
{
    /* bb_led_primary() returns NULL before bb_led_set_primary is called. */
    TEST_ASSERT_NULL(bb_led_primary());
}

void test_bb_led_set_primary_stores_handle(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_full, &g_mock, &h);
    bb_led_set_primary(h);
    TEST_ASSERT_EQUAL_PTR(h, bb_led_primary());
    bb_led_set_primary(NULL);
    bb_led_close(h);
}

void test_bb_led_set_primary_null_clears(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_full, &g_mock, &h);
    bb_led_set_primary(h);
    bb_led_set_primary(NULL);
    TEST_ASSERT_NULL(bb_led_primary());
    bb_led_close(h);
}

void test_bb_led_name_returns_driver_name(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_full, &g_mock, &h);
    /* mock_full has no .name set in the static struct; driver name is NULL for mock. */
    /* We test with a named driver. */
    static const bb_led_driver_t named_driver = {
        .name = "testled",
        .set_on = m_set_on,
        .set_brightness = m_set_brightness,
        .set_color = m_set_color,
        .flush = m_flush,
        .close = m_close,
        .caps  = BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS | BB_LED_CAP_RGB,
        .count = 1,
    };
    bb_led_handle_t h2;
    bb_led_handle_create(&named_driver, &g_mock, &h2);
    TEST_ASSERT_EQUAL_STRING("testled", bb_led_name(h2));
    bb_led_close(h);
    bb_led_close(h2);
}

void test_bb_led_name_null_handle_returns_null(void)
{
    TEST_ASSERT_NULL(bb_led_name(NULL));
}

/* ---------------------------------------------------------------------------
 * Tests: /api/info led extender (CONFIG_BB_LED_INFO)
 * --------------------------------------------------------------------------- */

#include "bb_led_info.h"
#include "bb_info.h"
#include "bb_info_test.h"
#include "bb_json.h"

/* Drivers with names for testing. */
static const bb_led_driver_t drv_apa102 = {
    .name  = "apa102",
    .set_on = m_set_on, .set_brightness = m_set_brightness,
    .set_color = m_set_color, .flush = m_flush, .close = m_close,
    .caps  = BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS | BB_LED_CAP_RGB,
    .count = 8,
};

static const bb_led_driver_t drv_pwm = {
    .name  = "pwm",
    .set_on = m_set_on, .set_brightness = m_set_brightness,
    .set_color = m_set_color, .flush = m_flush, .close = m_close,
    .caps  = BB_LED_CAP_ONOFF | BB_LED_CAP_BRIGHTNESS,
    .count = 1,
};

void test_bb_led_info_schema_in_assembled_schema(void)
{
    bb_led_register_info();
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"led\""),
                                 "led key not in assembled schema");
    TEST_ASSERT_NOT_NULL(strstr(schema, "\"present\""));
    TEST_ASSERT_NOT_NULL(strstr(schema, "\"type\""));
    TEST_ASSERT_NOT_NULL(strstr(schema, "\"count\""));
    TEST_ASSERT_NOT_NULL(strstr(schema, "\"rgb\""));
}

void test_bb_led_info_extender_no_primary_present_false(void)
{
    /* No primary set → present:false */
    bb_led_set_primary(NULL);
    bb_led_register_info();

    bb_json_t root = bb_json_obj_new();
    bb_info_invoke_extenders_for_test(root);

    bb_json_t led = bb_json_obj_get_item(root, "led");
    TEST_ASSERT_NOT_NULL_MESSAGE(led, "led key missing from extender output");

    bool present = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(led, "present", &present));
    TEST_ASSERT_FALSE(present);

    bb_json_free(root);
}

void test_bb_led_info_extender_rgb_primary_present_true(void)
{
    /* apa102 driver: rgb=true, count=8 */
    bb_led_handle_t h;
    bb_led_handle_create(&drv_apa102, &g_mock, &h);
    bb_led_set_primary(h);
    bb_led_register_info();

    bb_json_t root = bb_json_obj_new();
    bb_info_invoke_extenders_for_test(root);

    bb_json_t led = bb_json_obj_get_item(root, "led");
    TEST_ASSERT_NOT_NULL_MESSAGE(led, "led key missing");

    bool present = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(led, "present", &present));
    TEST_ASSERT_TRUE(present);

    char type[32] = {0};
    TEST_ASSERT_TRUE(bb_json_obj_get_string(led, "type", type, sizeof(type)));
    TEST_ASSERT_EQUAL_STRING("apa102", type);

    double count = 0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(led, "count", &count));
    TEST_ASSERT_EQUAL_DOUBLE(8.0, count);

    bool rgb = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(led, "rgb", &rgb));
    TEST_ASSERT_TRUE(rgb);

    bb_json_free(root);
    bb_led_set_primary(NULL);
    bb_led_close(h);
}

void test_bb_led_info_extender_pwm_primary_rgb_false(void)
{
    /* pwm driver: no RGB cap */
    bb_led_handle_t h;
    bb_led_handle_create(&drv_pwm, &g_mock, &h);
    bb_led_set_primary(h);
    bb_led_register_info();

    bb_json_t root = bb_json_obj_new();
    bb_info_invoke_extenders_for_test(root);

    bb_json_t led = bb_json_obj_get_item(root, "led");
    TEST_ASSERT_NOT_NULL_MESSAGE(led, "led key missing");

    bool present = false;
    bb_json_obj_get_bool(led, "present", &present);
    TEST_ASSERT_TRUE(present);

    bool rgb = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(led, "rgb", &rgb));
    TEST_ASSERT_FALSE(rgb);

    double count = 0;
    bb_json_obj_get_number(led, "count", &count);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, count);

    bb_json_free(root);
    bb_led_set_primary(NULL);
    bb_led_close(h);
}

/* ---------------------------------------------------------------------------
 * Tests: bb_led_enabled / bb_led_set_enabled
 * --------------------------------------------------------------------------- */

void test_bb_led_enabled_default_true(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_full, &g_mock, &h);
    TEST_ASSERT_TRUE(bb_led_enabled(h));
    bb_led_close(h);
}

void test_bb_led_set_enabled_false(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_full, &g_mock, &h);
    TEST_ASSERT_EQUAL(BB_OK, bb_led_set_enabled(h, false));
    TEST_ASSERT_FALSE(bb_led_enabled(h));
    bb_led_close(h);
}

void test_bb_led_set_enabled_roundtrip(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&mock_full, &g_mock, &h);
    bb_led_set_enabled(h, false);
    TEST_ASSERT_FALSE(bb_led_enabled(h));
    bb_led_set_enabled(h, true);
    TEST_ASSERT_TRUE(bb_led_enabled(h));
    bb_led_close(h);
}

void test_bb_led_set_enabled_null_returns_err(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_STATE, bb_led_set_enabled(NULL, false));
}

void test_bb_led_enabled_null_returns_false(void)
{
    TEST_ASSERT_FALSE(bb_led_enabled(NULL));
}

/* ---------------------------------------------------------------------------
 * Tests: bb_led_info emits enabled field
 * --------------------------------------------------------------------------- */

void test_bb_led_info_schema_includes_enabled(void)
{
    bb_led_register_info();
    const char *schema = bb_info_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"enabled\""),
                                 "enabled key not in assembled schema");
}

void test_bb_led_info_extender_enabled_true_by_default(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&drv_apa102, &g_mock, &h);
    bb_led_set_primary(h);
    bb_led_register_info();

    bb_json_t root = bb_json_obj_new();
    bb_info_invoke_extenders_for_test(root);

    bb_json_t led = bb_json_obj_get_item(root, "led");
    TEST_ASSERT_NOT_NULL_MESSAGE(led, "led key missing");

    bool enabled = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(led, "enabled", &enabled));
    TEST_ASSERT_TRUE(enabled);

    bb_json_free(root);
    bb_led_set_primary(NULL);
    bb_led_close(h);
}

void test_bb_led_info_extender_enabled_false_after_set(void)
{
    bb_led_handle_t h;
    bb_led_handle_create(&drv_apa102, &g_mock, &h);
    bb_led_set_enabled(h, false);
    bb_led_set_primary(h);
    bb_led_register_info();

    bb_json_t root = bb_json_obj_new();
    bb_info_invoke_extenders_for_test(root);

    bb_json_t led = bb_json_obj_get_item(root, "led");
    TEST_ASSERT_NOT_NULL_MESSAGE(led, "led key missing");

    bool enabled = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(led, "enabled", &enabled));
    TEST_ASSERT_FALSE(enabled);

    bb_json_free(root);
    bb_led_set_primary(NULL);
    bb_led_close(h);
}

