// Tests for bb_mqtt_client's /api/health "mqtt" section -- exercises
// bb_mqtt_client_health_section_desc/bb_mqtt_client_health_section_fill()
// and bb_mqtt_client_health_register() against the bb_health_section
// composer registry (B1-1099, PR-4 of the bb_health/bb_response migration
// chain, epic B1-1054). Folds the now-deleted bb_mqtt_info component's
// tests, ported onto the new seam -- structure/presence assertions only
// (mirrors test_bb_temp.c's idiom).

#include "unity.h"
#include "bb_mqtt_client.h"
#include "bb_serialize_json.h"

#include <string.h>

/* setUp/tearDown are called per-test by the test runner via test_main.c setUp(). */

/* ---- bb_mqtt_client_health_section_fill (bb_health_fill_fn adapter) ---- */

void test_bb_mqtt_health_section_fill_rejects_null_dst(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_mqtt_client_health_section_fill(NULL, NULL));
}

void test_bb_mqtt_health_section_fill_no_handle_enabled_false(void)
{
    /* bb_mqtt_client_default() returns NULL by default on host */
    bb_mqtt_client_health_section_snap_t snap;
    memset(&snap, 0xAA, sizeof(snap));
    bb_health_fill_args_t args = { .ctx = NULL };
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_health_section_fill(&snap, &args));

    TEST_ASSERT_FALSE(snap.enabled);
    TEST_ASSERT_FALSE(snap.connected);
}

void test_bb_mqtt_health_section_fill_handle_present_enabled_true(void)
{
    bb_mqtt_client_t h = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://broker.example.com:1883" };
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_default_set(h);
    /* host stub initialises connected=true */

    bb_mqtt_client_health_section_snap_t snap;
    memset(&snap, 0, sizeof(snap));
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_health_section_fill(&snap, NULL));

    TEST_ASSERT_TRUE(snap.enabled);
    TEST_ASSERT_TRUE(snap.connected);

    bb_mqtt_client_default_set(NULL);
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_health_section_fill_set_disconnected(void)
{
    bb_mqtt_client_t h = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://broker.example.com:1883" };
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_default_set(h);
    bb_mqtt_client_host_set_connected(h, false);

    bb_mqtt_client_health_section_snap_t snap;
    memset(&snap, 0, sizeof(snap));
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_health_section_fill(&snap, NULL));

    TEST_ASSERT_TRUE(snap.enabled);    /* handle present -> enabled */
    TEST_ASSERT_FALSE(snap.connected); /* explicitly disconnected */

    bb_mqtt_client_default_set(NULL);
    bb_mqtt_client_destroy(h);
}

void test_bb_mqtt_health_section_fill_reconnect_reflects_connected_true(void)
{
    bb_mqtt_client_t h = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://broker.example.com:1883" };
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_default_set(h);
    bb_mqtt_client_host_set_connected(h, false);
    bb_mqtt_client_host_set_connected(h, true);

    bb_mqtt_client_health_section_snap_t snap;
    memset(&snap, 0, sizeof(snap));
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_health_section_fill(&snap, NULL));

    TEST_ASSERT_TRUE(snap.connected);

    bb_mqtt_client_default_set(NULL);
    bb_mqtt_client_destroy(h);
}

/* ---- bb_mqtt_client_health_register (new bb_health_section seam) ---- */

void test_bb_mqtt_health_register_registers_into_new_table(void)
{
    bb_health_section_test_reset();

    bb_mqtt_client_health_register();

    const bb_health_section_t *stored = bb_health_section_test_find("mqtt");
    TEST_ASSERT_NOT_NULL(stored);
    TEST_ASSERT_EQUAL_STRING("mqtt", stored->name);
    TEST_ASSERT_EQUAL_PTR(&bb_mqtt_client_health_section_desc, stored->snap_desc);
    TEST_ASSERT_EQUAL_PTR(bb_mqtt_client_health_section_fill, stored->fill);

    bb_health_section_test_reset();
}

void test_bb_mqtt_health_register_schema_props_present(void)
{
    bb_health_section_test_reset();

    bb_mqtt_client_health_register();

    const bb_health_section_t *stored = bb_health_section_test_find("mqtt");
    TEST_ASSERT_NOT_NULL(stored);
    TEST_ASSERT_NOT_NULL(stored->schema_props);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(stored->schema_props, "\"enabled\""),
                                 "enabled key missing from mqtt schema fragment");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(stored->schema_props, "\"connected\""),
                                 "connected key missing from mqtt schema fragment");

    bb_health_section_test_reset();
}

/* ---- bb_mqtt_client_health_autoregister_init (registry hook) ---- */

void test_bb_mqtt_health_autoregister_init_registers_into_new_table(void)
{
    bb_health_section_test_reset();

    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_health_autoregister_init());

    const bb_health_section_t *stored = bb_health_section_test_find("mqtt");
    TEST_ASSERT_NOT_NULL(stored);

    bb_health_section_test_reset();
}

/* ---- structure byte-fidelity vs today's {enabled, connected} shape ---- */

void test_bb_mqtt_health_section_desc_wire_shape_disabled(void)
{
    bb_mqtt_client_health_section_snap_t snap;
    memset(&snap, 0, sizeof(snap));
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_health_section_fill(&snap, NULL));

    char buf[128];
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_serialize_json_render(&bb_mqtt_client_health_section_desc, &snap, buf, sizeof(buf), &len));

    TEST_ASSERT_NOT_NULL(strstr(buf, "\"enabled\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"connected\":false"));
}

void test_bb_mqtt_health_section_desc_wire_shape_enabled_connected(void)
{
    bb_mqtt_client_t h = NULL;
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://broker.example.com:1883" };
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_default_set(h);

    bb_mqtt_client_health_section_snap_t snap;
    memset(&snap, 0, sizeof(snap));
    TEST_ASSERT_EQUAL(BB_OK, bb_mqtt_client_health_section_fill(&snap, NULL));

    char buf[128];
    size_t len = 0;
    TEST_ASSERT_EQUAL(BB_OK,
        bb_serialize_json_render(&bb_mqtt_client_health_section_desc, &snap, buf, sizeof(buf), &len));

    TEST_ASSERT_NOT_NULL(strstr(buf, "\"enabled\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"connected\":true"));

    bb_mqtt_client_default_set(NULL);
    bb_mqtt_client_destroy(h);
}
