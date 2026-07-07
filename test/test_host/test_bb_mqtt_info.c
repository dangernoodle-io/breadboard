#include "unity.h"
#include "bb_mqtt_info.h"
#include "bb_mqtt_client.h"
#include "bb_health.h"
#include "bb_health_test.h"
#include "bb_json.h"

#include <string.h>

/* setUp/tearDown are called per-test by the test runner via test_main.c setUp(). */

/* ---- helpers ---- */

static bb_json_t invoke_sections(void)
{
    bb_mqtt_register_health();
    bb_json_t root = bb_json_obj_new();
    bb_health_invoke_sections_for_test(root);
    return root;
}

/* ---- tests: no handle (MQTT disabled/not started) ---- */

void test_bb_mqtt_health_no_handle_enabled_false(void)
{
    /* bb_mqtt_client_default() returns NULL by default on host */
    bb_json_t root = invoke_sections();

    bb_json_t mqtt = bb_json_obj_get_item(root, "mqtt");
    TEST_ASSERT_NOT_NULL_MESSAGE(mqtt, "mqtt key missing from health output");

    bool enabled = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(mqtt, "enabled", &enabled));
    TEST_ASSERT_FALSE(enabled);

    bb_json_free(root);
}

void test_bb_mqtt_health_no_handle_connected_false(void)
{
    bb_json_t root = invoke_sections();

    bb_json_t mqtt = bb_json_obj_get_item(root, "mqtt");
    TEST_ASSERT_NOT_NULL(mqtt);

    bool connected = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(mqtt, "connected", &connected));
    TEST_ASSERT_FALSE(connected);

    bb_json_free(root);
}

/* ---- tests: handle present, connected ---- */

void test_bb_mqtt_health_handle_present_enabled_true(void)
{
    bb_mqtt_client_t h = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://broker.example.com:1883" };
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_default_set(h);
    /* host stub initialises connected=true */

    bb_json_t root = invoke_sections();
    bb_json_t mqtt = bb_json_obj_get_item(root, "mqtt");
    TEST_ASSERT_NOT_NULL(mqtt);

    bool enabled = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(mqtt, "enabled", &enabled));
    TEST_ASSERT_TRUE(enabled);

    bb_json_free(root);
    bb_mqtt_client_default_set(NULL);
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_health_handle_connected_reflects_stub(void)
{
    bb_mqtt_client_t h = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://broker.example.com:1883" };
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_default_set(h);
    /* default connected=true on host stub */

    bb_json_t root = invoke_sections();
    bb_json_t mqtt = bb_json_obj_get_item(root, "mqtt");
    TEST_ASSERT_NOT_NULL(mqtt);

    bool connected = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(mqtt, "connected", &connected));
    TEST_ASSERT_TRUE(connected);

    bb_json_free(root);
    bb_mqtt_client_default_set(NULL);
    bb_mqtt_client_destroy(h);
}

/* ---- tests: toggling connected state ---- */

void test_bb_mqtt_health_set_disconnected(void)
{
    bb_mqtt_client_t h = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://broker.example.com:1883" };
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_default_set(h);
    bb_mqtt_client_host_set_connected(h, false);

    bb_json_t root = invoke_sections();
    bb_json_t mqtt = bb_json_obj_get_item(root, "mqtt");
    TEST_ASSERT_NOT_NULL(mqtt);

    bool enabled = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(mqtt, "enabled", &enabled));
    TEST_ASSERT_TRUE(enabled);   /* handle present → enabled */

    bool connected = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(mqtt, "connected", &connected));
    TEST_ASSERT_FALSE(connected); /* explicitly disconnected */

    bb_json_free(root);
    bb_mqtt_client_default_set(NULL);
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_health_reconnect_reflects_connected_true(void)
{
    bb_mqtt_client_t h = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://broker.example.com:1883" };
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_default_set(h);
    bb_mqtt_client_host_set_connected(h, false);
    bb_mqtt_client_host_set_connected(h, true);

    bb_json_t root = invoke_sections();
    bb_json_t mqtt = bb_json_obj_get_item(root, "mqtt");
    TEST_ASSERT_NOT_NULL(mqtt);

    bool connected = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(mqtt, "connected", &connected));
    TEST_ASSERT_TRUE(connected);

    bb_json_free(root);
    bb_mqtt_client_default_set(NULL);
    bb_mqtt_client_destroy(h);
}

/* ---- test: schema in assembled /api/health schema ---- */

void test_bb_mqtt_health_schema_fragment_present(void)
{
    bb_mqtt_register_health();
    const char *schema = bb_health_get_assembled_schema();
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, "\"mqtt\""),
                                 "mqtt key not in health schema");
    TEST_ASSERT_NOT_NULL(strstr(schema, "\"enabled\""));
    TEST_ASSERT_NOT_NULL(strstr(schema, "\"connected\""));
}
