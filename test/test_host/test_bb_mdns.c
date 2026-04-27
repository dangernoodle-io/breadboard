#include "unity.h"
#include "bb_nv.h"
#include "bb_mdns.h"
#include "bb_mdns_host_test_hooks.h"
#include <string.h>

void test_bb_mdns_browse_start_null_service(void)
{
    bb_err_t err = bb_mdns_browse_start(NULL, "_tcp", NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_mdns_browse_start_null_proto(void)
{
    bb_err_t err = bb_mdns_browse_start("_taipanminer", NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_mdns_browse_stop_unstarted(void)
{
    bb_err_t err = bb_mdns_browse_stop("_taipanminer", "_tcp");
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_mdns_browse_start_valid(void)
{
    bb_err_t err = bb_mdns_browse_start("_taipanminer", "_tcp", NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_mdns_browse_stop_valid(void)
{
    bb_mdns_browse_start("_test", "_tcp", NULL, NULL, NULL);
    bb_err_t err = bb_mdns_browse_stop("_test", "_tcp");
    TEST_ASSERT_EQUAL(BB_OK, err);
}

void test_bb_mdns_browse_stop_null_service(void)
{
    bb_err_t err = bb_mdns_browse_stop(NULL, "_tcp");
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

void test_bb_mdns_browse_stop_null_proto(void)
{
    bb_err_t err = bb_mdns_browse_stop("_taipanminer", NULL);
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, err);
}

/* bb_mdns_announce host stub tests.
 * Timer-based auto-coalesce (bb_mdns_set_txt arming esp_timer) is ESP-IDF
 * hardware behaviour — verified by flashing, not here. */

void test_mdns_announce_explicit_increments_counter(void)
{
    bb_mdns_host_reset();
    bb_mdns_announce();
    TEST_ASSERT_EQUAL(1, bb_mdns_host_announce_count());
    bb_mdns_announce();
    TEST_ASSERT_EQUAL(2, bb_mdns_host_announce_count());
}

void test_mdns_set_txt_does_not_announce_immediately(void)
{
    /* On host, set_txt increments the txt counter but NOT the announce counter —
     * coalescing is ESP-only timer behaviour; the announce counter only moves
     * on explicit bb_mdns_announce() calls. */
    bb_mdns_host_reset();
    bb_mdns_set_txt("version", "v0.14.1");
    bb_mdns_set_txt("state", "mining");
    TEST_ASSERT_EQUAL(2, bb_mdns_host_set_txt_count());
    TEST_ASSERT_EQUAL(0, bb_mdns_host_announce_count());
}

void test_mdns_set_txt_null_key_is_safe(void)
{
    bb_mdns_host_reset();
    bb_mdns_set_txt(NULL, "value");
    TEST_ASSERT_EQUAL(0, bb_mdns_host_set_txt_count());
}

void test_mdns_set_txt_null_value_is_safe(void)
{
    bb_mdns_host_reset();
    bb_mdns_set_txt("key", NULL);
    TEST_ASSERT_EQUAL(0, bb_mdns_host_set_txt_count());
}

void test_mdns_host_reset_clears_counters(void)
{
    bb_mdns_announce();
    bb_mdns_set_txt("k", "v");
    bb_mdns_host_reset();
    TEST_ASSERT_EQUAL(0, bb_mdns_host_announce_count());
    TEST_ASSERT_EQUAL(0, bb_mdns_host_set_txt_count());
}

/* Dispatch hook tests */

static int s_peer_fired = 0;
static int s_removed_fired = 0;
static char s_last_instance[64];
static uint16_t s_last_port = 0;

static void test_peer_cb(const bb_mdns_peer_t *peer, void *ctx)
{
    (void)ctx;
    s_peer_fired++;
    strncpy(s_last_instance, peer->instance_name ? peer->instance_name : "",
            sizeof(s_last_instance) - 1);
    s_last_instance[sizeof(s_last_instance) - 1] = '\0';
    s_last_port = peer->port;
}

static void test_removed_cb(const char *instance_name, void *ctx)
{
    (void)ctx;
    s_removed_fired++;
    strncpy(s_last_instance, instance_name ? instance_name : "",
            sizeof(s_last_instance) - 1);
    s_last_instance[sizeof(s_last_instance) - 1] = '\0';
}

void test_bb_mdns_dispatch_peer_fires_callback(void)
{
    s_peer_fired = 0;
    s_last_port  = 0;
    bb_mdns_browse_start("_acme", "_tcp", test_peer_cb, NULL, NULL);
    bb_mdns_peer_t peer = {
        .instance_name = "acme-device-01",
        .hostname      = "acme.local",
        .ip4           = "192.168.1.10",
        .port          = 4242,
        .txt           = NULL,
        .txt_count     = 0,
    };
    bb_err_t err = bb_mdns_host_dispatch_peer("_acme", "_tcp", &peer);
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(1, s_peer_fired);
    TEST_ASSERT_EQUAL_STRING("acme-device-01", s_last_instance);
    TEST_ASSERT_EQUAL(4242, s_last_port);
    bb_mdns_browse_stop("_acme", "_tcp");
}

void test_bb_mdns_dispatch_removed_fires_callback(void)
{
    s_removed_fired = 0;
    s_last_instance[0] = '\0';
    bb_mdns_browse_start("_acme", "_tcp", NULL, test_removed_cb, NULL);
    bb_err_t err = bb_mdns_host_dispatch_removed("_acme", "_tcp", "acme-device-01");
    TEST_ASSERT_EQUAL(BB_OK, err);
    TEST_ASSERT_EQUAL(1, s_removed_fired);
    TEST_ASSERT_EQUAL_STRING("acme-device-01", s_last_instance);
    bb_mdns_browse_stop("_acme", "_tcp");
}

void test_bb_mdns_dispatch_peer_null_cb_no_crash(void)
{
    bb_mdns_browse_start("_acme", "_tcp", NULL, NULL, NULL);
    bb_mdns_peer_t peer = {
        .instance_name = "acme-device-01",
        .hostname      = NULL,
        .ip4           = NULL,
        .port          = 80,
        .txt           = NULL,
        .txt_count     = 0,
    };
    bb_err_t err = bb_mdns_host_dispatch_peer("_acme", "_tcp", &peer);
    TEST_ASSERT_EQUAL(BB_OK, err);
    bb_mdns_browse_stop("_acme", "_tcp");
}

void test_bb_mdns_dispatch_removed_null_cb_no_crash(void)
{
    bb_mdns_browse_start("_acme", "_tcp", NULL, NULL, NULL);
    bb_err_t err = bb_mdns_host_dispatch_removed("_acme", "_tcp", "acme-device-01");
    TEST_ASSERT_EQUAL(BB_OK, err);
    bb_mdns_browse_stop("_acme", "_tcp");
}

void test_bb_mdns_dispatch_no_subscription_returns_ok(void)
{
    bb_mdns_peer_t peer = {
        .instance_name = "orphan-device",
        .hostname      = NULL,
        .ip4           = NULL,
        .port          = 0,
        .txt           = NULL,
        .txt_count     = 0,
    };
    bb_err_t err = bb_mdns_host_dispatch_peer("_nosub", "_tcp", &peer);
    TEST_ASSERT_EQUAL(BB_OK, err);
    err = bb_mdns_host_dispatch_removed("_nosub", "_tcp", "orphan-device");
    TEST_ASSERT_EQUAL(BB_OK, err);
}
