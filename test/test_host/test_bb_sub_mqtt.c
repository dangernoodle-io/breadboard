// Tests for bb_sub_mqtt (B1-490): MQTT ingress adapter for bb_sub.
//
// Covers:
//  - bb_sub_mqtt_add_topic bookkeeping (happy path, NULL/empty arg, overflow)
//  - bb_sub_mqtt_init with no default MQTT client is a safe no-op
//  - bb_sub_mqtt_init wires bb_mqtt_client_on_message -> bb_sub_route, and an
//    injected message lands in bb_cache
//  - self-exclusion: a message on this device's own hostname topic segment
//    ("<prefix>/<hostname>/<subtopic>") is dropped by default; other
//    hostnames pass through. bb_sub_mqtt_set_ignore_self(false) opts out.
#include "unity.h"
#include "bb_sub_mqtt.h"
#include "bb_sub.h"
#include "bb_mqtt_client.h"
#include "bb_nv.h"
#include "bb_cache.h"
#include "bb_json.h"
#include "bb_transport_health.h"

#include <stdio.h>
#include <string.h>

// Declared in bb_cache_espidf.c under BB_CACHE_TESTING; not in the public
// header (mirrors test_bb_cache_fidelity.c).
void bb_cache_reset_for_test(void);

// ---------------------------------------------------------------------------
// setUp
// ---------------------------------------------------------------------------

static bb_mqtt_client_t make_default_client(void)
{
    bb_mqtt_client_cfg_t cfg = { .uri = "mqtt://localhost:1883" };
    bb_mqtt_client_t h = NULL;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_mqtt_client_init(&cfg, &h));
    bb_mqtt_client_default_set(h);
    return h;
}

static void reset_all(void)
{
    bb_cache_reset_for_test();
    bb_sub_reset_for_test();
    bb_sub_mqtt_reset_for_test();
    bb_mqtt_client_default_set(NULL);
    bb_nv_config_set_hostname("myhost");
}

// ---------------------------------------------------------------------------
// bb_sub_mqtt_add_topic
// ---------------------------------------------------------------------------

void test_bb_sub_mqtt_add_topic_null_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_sub_mqtt_add_topic(NULL));
}

void test_bb_sub_mqtt_add_topic_empty_returns_invalid_arg(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_sub_mqtt_add_topic(""));
}

void test_bb_sub_mqtt_add_topic_happy_path(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_add_topic("a/#"));
}

void test_bb_sub_mqtt_add_topic_overflow_returns_no_space(void)
{
    reset_all();
    char filter[16];
    for (int i = 0; i < BB_SUB_MQTT_MAX_TOPICS; i++) {
        snprintf(filter, sizeof(filter), "f/%d", i);
        TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_add_topic(filter));
    }
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, bb_sub_mqtt_add_topic("one/more"));
}

// ---------------------------------------------------------------------------
// bb_sub_mqtt_add_topic: self-exclusion shape-mismatch warn (MED finding)
// ---------------------------------------------------------------------------

void test_bb_sub_mqtt_add_topic_shape_mismatch_warns_once(void)
{
    reset_all();
    // ignore_self defaults true; "a/#" has only 1 '/' -> not a plausible
    // "<prefix>/<hostname>/<subtopic>" shape -> warns once.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_add_topic("a/#"));
    // Second non-default-shaped filter: warn is one-time (s_shape_warned
    // guard), exercises the already-warned branch.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_add_topic("b/#"));
    TEST_ASSERT_EQUAL_INT(2, bb_sub_mqtt_test_topic_count());
}

void test_bb_sub_mqtt_add_topic_no_slash_filter_is_non_default_shape(void)
{
    reset_all();
    // No '/' at all -> filter_has_default_shape's !first branch.
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_add_topic("onlyoneword"));
}

void test_bb_sub_mqtt_add_topic_shape_mismatch_no_warn_when_ignore_self_false(void)
{
    reset_all();
    bb_sub_mqtt_set_ignore_self(false);
    // ignore_self is off -> shape check short-circuits (no warn attempted).
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_add_topic("a/#"));
}

// ---------------------------------------------------------------------------
// bb_sub_mqtt_init with no default MQTT client
// ---------------------------------------------------------------------------

void test_bb_sub_mqtt_init_no_default_client_is_safe(void)
{
    reset_all();
    bb_mqtt_client_default_set(NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());
}

void test_bb_sub_mqtt_init_called_twice_reuses_loaded_kconfig(void)
{
    // Second bb_sub_mqtt_init() call hits load_kconfig_default's
    // s_kconfig_loaded-already-true early return.
    reset_all();
    bb_mqtt_client_t h = make_default_client();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());
    TEST_ASSERT_EQUAL_INT(1, bb_sub_mqtt_test_topic_count());
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());
    TEST_ASSERT_EQUAL_INT(1, bb_sub_mqtt_test_topic_count());
    bb_mqtt_client_destroy(h);
}

void test_bb_sub_mqtt_explicit_add_topic_skips_kconfig_default(void)
{
    // Caller-supplied filter registered before init() -> load_kconfig_default
    // sees s_topic_count > 0 and returns without parsing CONFIG_BB_SUB_MQTT_TOPICS.
    reset_all();
    bb_mqtt_client_t h = make_default_client();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_add_topic("custom/+/topic"));
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());
    TEST_ASSERT_EQUAL_INT(1, bb_sub_mqtt_test_topic_count());
    bb_mqtt_client_destroy(h);
}

void test_bb_sub_mqtt_empty_topics_cfg_configures_nothing(void)
{
    // Empty override string hits load_kconfig_default's !cfg[0] early return.
    reset_all();
    bb_mqtt_client_t h = make_default_client();
    bb_sub_mqtt_test_set_topics_cfg("");
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());
    TEST_ASSERT_EQUAL_INT(0, bb_sub_mqtt_test_topic_count());
    bb_mqtt_client_destroy(h);
}

// ---------------------------------------------------------------------------
// End-to-end: bb_sub_mqtt_init wires receive -> bb_sub_route -> bb_cache
// ---------------------------------------------------------------------------

void test_bb_sub_mqtt_init_routes_injected_message_into_cache(void)
{
    reset_all();
    bb_mqtt_client_t h = make_default_client();

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());

    bb_mqtt_client_host_inject_message(h, "metrics/otherhost/meta", "{\"v\":7}", 7);

    char buf[128];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("metrics/otherhost/meta", buf, sizeof(buf), &len));
    bb_json_t obj = bb_json_parse(buf, len);
    TEST_ASSERT_NOT_NULL(obj);
    // bb_cache_get_serialized now wraps every read in the {"ts_ms":N,"data":
    // {...}} envelope (B1-570 PR-3) — the routed fields live under "data".
    bb_json_t data = bb_json_obj_get_item(obj, "data");
    double v = 0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(data, "v", &v));
    TEST_ASSERT_EQUAL_INT(7, (int)v);
    bb_json_free(obj);

    bb_mqtt_client_destroy(h);
}

// ---------------------------------------------------------------------------
// Self-exclusion
// ---------------------------------------------------------------------------

void test_bb_sub_mqtt_ignores_own_hostname_topic_by_default(void)
{
    reset_all();
    bb_mqtt_client_t h = make_default_client();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());

    // hostname == "myhost" (set in reset_all); topic's 2nd segment matches.
    bb_mqtt_client_host_inject_message(h, "metrics/myhost/meta", "{\"x\":1}", 7);

    char buf[64];
    size_t len = 0;
    bb_err_t rc = bb_cache_get_serialized("metrics/myhost/meta", buf, sizeof(buf), &len);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, rc);   // never routed -> never registered

    bb_mqtt_client_destroy(h);
}

void test_bb_sub_mqtt_allows_other_hostname_topic_by_default(void)
{
    reset_all();
    bb_mqtt_client_t h = make_default_client();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());

    bb_mqtt_client_host_inject_message(h, "metrics/otherhost/meta", "{\"x\":1}", 7);

    char buf[64];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("metrics/otherhost/meta", buf, sizeof(buf), &len));

    bb_mqtt_client_destroy(h);
}

void test_bb_sub_mqtt_same_length_different_hostname_not_excluded(void)
{
    // hostname == "myhost" (6 chars). Topic's 2nd segment is also 6 chars
    // ("myhosx") so seg1_len == host_len, but the content differs -> the
    // strncmp() branch of topic_is_own_hostname must take its non-zero
    // (not-equal) path, not just the length-mismatch path already covered
    // by test_bb_sub_mqtt_allows_other_hostname_topic_by_default (9 chars).
    reset_all();
    bb_mqtt_client_t h = make_default_client();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());

    bb_mqtt_client_host_inject_message(h, "metrics/myhosx/meta", "{\"x\":1}", 7);

    char buf[64];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("metrics/myhosx/meta", buf, sizeof(buf), &len));

    bb_mqtt_client_destroy(h);
}

void test_bb_sub_mqtt_set_ignore_self_false_ingests_own_topic(void)
{
    reset_all();
    bb_mqtt_client_t h = make_default_client();
    bb_sub_mqtt_set_ignore_self(false);
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());

    bb_mqtt_client_host_inject_message(h, "metrics/myhost/meta", "{\"x\":9}", 7);

    char buf[64];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("metrics/myhost/meta", buf, sizeof(buf), &len));

    bb_mqtt_client_destroy(h);
}

void test_bb_sub_mqtt_ignore_self_no_slash_topic_not_excluded(void)
{
    // A topic with no '/' has no 2nd segment to compare — must not be
    // treated as a self-match (and must route normally).
    reset_all();
    bb_mqtt_client_t h = make_default_client();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());

    bb_mqtt_client_host_inject_message(h, "myhost", "{\"x\":1}", 7);

    char buf[64];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_get_serialized("myhost", buf, sizeof(buf), &len));

    bb_mqtt_client_destroy(h);
}

void test_bb_sub_mqtt_ignore_self_two_segment_topic_no_trailing_slash(void)
{
    // "metrics/myhost" has exactly 2 segments (no 3rd '/') -> exercises the
    // seg1_end==NULL branch (strlen(seg1) instead of pointer-diff length).
    // Still a self-match (2nd segment == hostname) so it's excluded.
    reset_all();
    bb_mqtt_client_t h = make_default_client();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());

    bb_mqtt_client_host_inject_message(h, "metrics/myhost", "{\"x\":1}", 7);

    char buf[64];
    size_t len = 0;
    bb_err_t rc = bb_cache_get_serialized("metrics/myhost", buf, sizeof(buf), &len);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, rc);

    bb_mqtt_client_destroy(h);
}

void test_bb_sub_mqtt_empty_hostname_fails_open_and_routes(void)
{
    // Fail-open contract (bb_sub_mqtt.h): an empty hostname means self-
    // exclusion never matches -> topic_is_own_hostname's !hostname[0] guard.
    reset_all();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_nv_config_factory_reset());
    bb_mqtt_client_t h = make_default_client();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());

    bb_mqtt_client_host_inject_message(h, "metrics//meta", "{\"x\":1}", 7);

    char buf[64];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("metrics//meta", buf, sizeof(buf), &len));

    bb_mqtt_client_destroy(h);
}

// ---------------------------------------------------------------------------
// Subscribe-failure branch (MED finding)
// ---------------------------------------------------------------------------

void test_bb_sub_mqtt_init_subscribe_failure_warns_and_continues(void)
{
    reset_all();
    bb_mqtt_client_t h = make_default_client();
    bb_mqtt_client_host_set_subscribe_fail(h, true);

    // bb_mqtt_client_subscribe fails for the one configured filter; bb_sub_mqtt_init
    // still returns BB_OK (warn-and-continue, not fatal).
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());
    TEST_ASSERT_EQUAL_INT(1, bb_sub_mqtt_test_topic_count());

    bb_mqtt_client_host_set_subscribe_fail(h, false);
    bb_mqtt_client_destroy(h);
}

// ---------------------------------------------------------------------------
// Multi-topic parse (MED finding)
// ---------------------------------------------------------------------------

void test_bb_sub_mqtt_multi_topic_config_parses_and_subscribes_all(void)
{
    reset_all();
    bb_mqtt_client_t h = make_default_client();
    bb_sub_mqtt_test_set_topics_cfg("metrics/+/meta, status/+/online");

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());
    TEST_ASSERT_EQUAL_INT(2, bb_sub_mqtt_test_topic_count());

    // Both filters actually route -> proves the subscribe loop processed
    // every parsed entry (not just the first/last).
    bb_mqtt_client_host_inject_message(h, "metrics/otherhost/meta", "{\"a\":1}", 7);
    bb_mqtt_client_host_inject_message(h, "status/otherhost/online", "{\"b\":2}", 7);

    char buf[64];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("metrics/otherhost/meta", buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("status/otherhost/online", buf, sizeof(buf), &len));

    bb_mqtt_client_destroy(h);
}

// ---------------------------------------------------------------------------
// bb_transport_health register + mark_activity wiring (INFERRED, OBSERVE-ONLY)
// ---------------------------------------------------------------------------

void test_bb_sub_mqtt_genuine_message_marks_ingress_activity(void)
{
    reset_all();
    bb_transport_health_reset_for_test();
    bb_sub_mqtt_reset_transport_health_for_test();
    bb_mqtt_client_t h = make_default_client();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());

    bb_mqtt_client_host_inject_message(h, "metrics/otherhost/meta", "{\"x\":1}", 7);

    bb_transport_health_snapshot_t snaps[BB_TRANSPORT_HEALTH_MAX_SLOTS];
    size_t n = bb_transport_health_snapshot_all(snaps, BB_TRANSPORT_HEALTH_MAX_SLOTS);
    bool found = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(snaps[i].name, "mqtt_sub") == 0) {
            found = true;
            TEST_ASSERT_EQUAL_INT(BB_TRANSPORT_INFERRED, snaps[i].cls);
            TEST_ASSERT_EQUAL_UINT32(1, snaps[i].rx_count);
            TEST_ASSERT_TRUE(snaps[i].last_rx_ms > 0);
        }
    }
    TEST_ASSERT_TRUE(found);

    bb_mqtt_client_destroy(h);
}

// Lazy register-once: two genuine inbound frames must register exactly one
// "mqtt_sub" slot, not one per message.
void test_bb_sub_mqtt_genuine_messages_register_transport_health_once(void)
{
    reset_all();
    bb_transport_health_reset_for_test();
    bb_sub_mqtt_reset_transport_health_for_test();
    bb_mqtt_client_t h = make_default_client();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());

    bb_mqtt_client_host_inject_message(h, "metrics/otherhost/meta", "{\"x\":1}", 7);
    bb_mqtt_client_host_inject_message(h, "metrics/otherhost/meta", "{\"x\":2}", 7);

    bb_transport_health_snapshot_t snaps[BB_TRANSPORT_HEALTH_MAX_SLOTS];
    size_t n = bb_transport_health_snapshot_all(snaps, BB_TRANSPORT_HEALTH_MAX_SLOTS);
    int mqtt_sub_slots = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(snaps[i].name, "mqtt_sub") == 0) {
            mqtt_sub_slots++;
            TEST_ASSERT_EQUAL_UINT32(2, snaps[i].rx_count);
        }
    }
    TEST_ASSERT_EQUAL_INT(1, mqtt_sub_slots);

    bb_mqtt_client_destroy(h);
}

// A self-filtered (own-hostname) message never reaches on_mqtt_message's
// mark_activity call — rx_count must stay unchanged (no slot even created).
void test_bb_sub_mqtt_self_filtered_message_does_not_mark_activity(void)
{
    reset_all();
    bb_transport_health_reset_for_test();
    bb_sub_mqtt_reset_transport_health_for_test();
    bb_mqtt_client_t h = make_default_client();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());

    // hostname == "myhost" (set in reset_all) -> self-filtered, dropped
    // before the mark_activity call.
    bb_mqtt_client_host_inject_message(h, "metrics/myhost/meta", "{\"x\":1}", 7);

    bb_transport_health_snapshot_t snaps[BB_TRANSPORT_HEALTH_MAX_SLOTS];
    size_t n = bb_transport_health_snapshot_all(snaps, BB_TRANSPORT_HEALTH_MAX_SLOTS);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(strcmp(snaps[i].name, "mqtt_sub") != 0);
    }

    bb_mqtt_client_destroy(h);
}

// INFERRED slots must never contribute to authoritative_counts() — the
// observe-only guarantee.
void test_bb_sub_mqtt_transport_health_excluded_from_authoritative_counts(void)
{
    reset_all();
    bb_transport_health_reset_for_test();
    bb_sub_mqtt_reset_transport_health_for_test();
    bb_mqtt_client_t h = make_default_client();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());

    bb_mqtt_client_host_inject_message(h, "metrics/otherhost/meta", "{\"x\":1}", 7);

    int enabled = -1, failing = -1;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_transport_health_authoritative_counts(&enabled, &failing));
    TEST_ASSERT_EQUAL_INT(0, enabled);
    TEST_ASSERT_EQUAL_INT(0, failing);

    bb_mqtt_client_destroy(h);
}

// When the bb_transport_health slot pool is exhausted, the lazy register
// inside on_mqtt_message must fail gracefully: the message still routes
// normally (bb_cache updated), but no "mqtt_sub" slot is created.
void test_bb_sub_mqtt_exhausted_transport_health_degrades_gracefully(void)
{
    reset_all();
    bb_transport_health_reset_for_test();
    bb_sub_mqtt_reset_transport_health_for_test();

    bb_transport_handle_t dummy;
    for (int i = 0; i < BB_TRANSPORT_HEALTH_MAX_SLOTS; i++) {
        TEST_ASSERT_EQUAL_INT(BB_OK,
            bb_transport_health_register("filler", BB_TRANSPORT_AUTHORITATIVE, &dummy));
    }

    bb_mqtt_client_t h = make_default_client();
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_sub_mqtt_init());

    bb_mqtt_client_host_inject_message(h, "metrics/otherhost/meta", "{\"v\":7}", 7);

    char buf[64];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_get_serialized("metrics/otherhost/meta", buf, sizeof(buf), &len));

    bb_transport_health_snapshot_t snaps[BB_TRANSPORT_HEALTH_MAX_SLOTS];
    size_t n = bb_transport_health_snapshot_all(snaps, BB_TRANSPORT_HEALTH_MAX_SLOTS);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(strcmp(snaps[i].name, "mqtt_sub") != 0);
    }

    bb_mqtt_client_destroy(h);
}
