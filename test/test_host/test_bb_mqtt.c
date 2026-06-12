// Tests for bb_mqtt host stub:
// - publish captures topic/payload/qos/retain
// - client_id default (hostname) / override / empty (null) resolution
// - is_connected flag settable via bb_mqtt_host_set_connected
#include "unity.h"
#include "bb_mqtt.h"
#include "bb_nv.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bb_mqtt_t make_client(const char *uri, const char *client_id)
{
    bb_mqtt_cfg_t cfg = {
        .uri       = uri ? uri : "mqtt://localhost:1883",
        .client_id = client_id,
        .tls       = false,
    };
    bb_mqtt_t h = NULL;
    bb_err_t rc = bb_mqtt_init(&cfg, &h);
    TEST_ASSERT_EQUAL_INT(BB_OK, rc);
    TEST_ASSERT_NOT_NULL(h);
    return h;
}

// ---------------------------------------------------------------------------
// Publish capture tests
// ---------------------------------------------------------------------------

void test_bb_mqtt_publish_captures_topic(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_publish(h, "test/topic", "hello", -1, 0, false);
    const bb_mqtt_host_pub_t *p = bb_mqtt_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("test/topic", p->topic);
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_publish_captures_payload(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_publish(h, "t", "my-payload", -1, 0, false);
    const bb_mqtt_host_pub_t *p = bb_mqtt_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("my-payload", p->payload);
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_publish_captures_qos_retain(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_publish(h, "t", "v", -1, 1, true);
    const bb_mqtt_host_pub_t *p = bb_mqtt_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT(1, p->qos);
    TEST_ASSERT_TRUE(p->retain);
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_publish_count_increments(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_host_pub_count(h));
    bb_mqtt_publish(h, "a", "1", -1, 0, false);
    bb_mqtt_publish(h, "b", "2", -1, 0, false);
    TEST_ASSERT_EQUAL_INT(2, bb_mqtt_host_pub_count(h));
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_publish_last_is_most_recent(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_publish(h, "first",  "1", -1, 0, false);
    bb_mqtt_publish(h, "second", "2", -1, 0, false);
    const bb_mqtt_host_pub_t *p = bb_mqtt_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("second", p->topic);
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_publish_explicit_len(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_publish(h, "t", "hello world", 5, 0, false);
    const bb_mqtt_host_pub_t *p = bb_mqtt_host_last_pub(h);
    TEST_ASSERT_NOT_NULL(p);
    // len=5 → "hello"
    TEST_ASSERT_EQUAL_INT(0, strncmp("hello", p->payload, 5));
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_host_reset_clears_pubs(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_publish(h, "t", "v", -1, 0, false);
    bb_mqtt_host_reset(h);
    TEST_ASSERT_EQUAL_INT(0, bb_mqtt_host_pub_count(h));
    TEST_ASSERT_NULL(bb_mqtt_host_last_pub(h));
    bb_mqtt_destroy(h);
}

// ---------------------------------------------------------------------------
// client_id resolution tests (host stub does not enforce hostname lookup,
// but init must succeed for all three modes)
// ---------------------------------------------------------------------------

void test_bb_mqtt_init_client_id_null_uses_hostname(void)
{
    // NULL client_id → default (hostname); init must succeed.
    bb_mqtt_t h = make_client(NULL, NULL);  // client_id=NULL
    TEST_ASSERT_NOT_NULL(h);
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_init_client_id_override(void)
{
    // Non-empty string → override.
    bb_mqtt_t h = make_client(NULL, "my-device-01");
    TEST_ASSERT_NOT_NULL(h);
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_init_client_id_empty_broker_assigned(void)
{
    // Empty string → set_null_client_id (broker assigns ID); init must succeed.
    bb_mqtt_t h = make_client(NULL, "");
    TEST_ASSERT_NOT_NULL(h);
    bb_mqtt_destroy(h);
}

// ---------------------------------------------------------------------------
// is_connected flag tests
// ---------------------------------------------------------------------------

void test_bb_mqtt_is_connected_default_true(void)
{
    // Host stub starts connected=true so publish tests work without extra setup.
    bb_mqtt_t h = make_client(NULL, NULL);
    TEST_ASSERT_TRUE(bb_mqtt_is_connected(h));
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_is_connected_set_false(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_host_set_connected(h, false);
    TEST_ASSERT_FALSE(bb_mqtt_is_connected(h));
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_is_connected_set_true(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_host_set_connected(h, false);
    bb_mqtt_host_set_connected(h, true);
    TEST_ASSERT_TRUE(bb_mqtt_is_connected(h));
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_is_connected_null_returns_false(void)
{
    TEST_ASSERT_FALSE(bb_mqtt_is_connected(NULL));
}

// ---------------------------------------------------------------------------
// Error cases
// ---------------------------------------------------------------------------

void test_bb_mqtt_init_null_cfg_returns_invalid_arg(void)
{
    bb_mqtt_t h = NULL;
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_mqtt_init(NULL, &h));
}

void test_bb_mqtt_init_null_out_returns_invalid_arg(void)
{
    bb_mqtt_cfg_t cfg = { .uri = "mqtt://localhost" };
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_mqtt_init(&cfg, NULL));
}

void test_bb_mqtt_destroy_null_is_safe(void)
{
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_destroy(NULL));
}

// ---------------------------------------------------------------------------
// bb_mqtt_default / bb_mqtt_default_set tests
// ---------------------------------------------------------------------------

void test_bb_mqtt_default_returns_null_initially(void)
{
    // Host stub starts with NULL default (no autoregister on host).
    // Reset to NULL first in case a prior test left it set.
    bb_mqtt_default_set(NULL);
    TEST_ASSERT_NULL(bb_mqtt_default());
}

void test_bb_mqtt_default_returns_set_handle(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_default_set(h);
    TEST_ASSERT_EQUAL_PTR(h, bb_mqtt_default());
    bb_mqtt_default_set(NULL);
    bb_mqtt_destroy(h);
}

void test_bb_mqtt_default_cleared_by_set_null(void)
{
    bb_mqtt_t h = make_client(NULL, NULL);
    bb_mqtt_default_set(h);
    bb_mqtt_default_set(NULL);
    TEST_ASSERT_NULL(bb_mqtt_default());
    bb_mqtt_destroy(h);
}
