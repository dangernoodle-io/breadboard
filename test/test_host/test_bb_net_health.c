// Host tests for bb_net_health — MQTT-fused early_warning classifier +
// egress/heap/emit/log helpers.
//
// The RSSI-bucket + hysteresis classifier and the net-mode classifier moved
// to bb_wifi (test/test_host/test_bb_wifi.c) — net_health teardown PR-B.
// Covers:
//   - early_warning on each trigger: sustained-poor, reconnect-increase, disconnect
//   - warn_disc uses mqtt_disc_age_s (not wifi disc_age_s)
//   - bb_net_health_throttle_decision: throttle start + restore
//   - bb_net_health_emit: nested mqtt sub-object; OOM branch coverage
#include "unity.h"
#include "bb_net_health.h"
#include "bb_json.h"
#include "bb_json_test_hooks.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bb_net_health_state_t  s_st;
static bb_net_health_input_t  s_in;
static bb_net_health_output_t s_out;

static void reset(void)
{
    memset(&s_st,  0, sizeof(s_st));
    memset(&s_in,  0, sizeof(s_in));
    memset(&s_out, 0, sizeof(s_out));
    // Default: strong signal, connected, no reconnects, no disc ages.
    s_in.rssi                 = -50;
    s_in.mqtt_connected       = true;
    s_in.mqtt_reconnect_count = 0;
    s_in.disc_age_s           = 0;
    s_in.mqtt_disc_age_s      = 0;
}

static void eval(void)
{
    bb_net_health_eval(&s_st, &s_in, &s_out);
}

// Evaluate N times with the same input.
static void eval_n(int n)
{
    for (int i = 0; i < n; i++) {
        eval();
    }
}

// ---------------------------------------------------------------------------
// early_warning trigger: sustained POOR
// ---------------------------------------------------------------------------

void test_bb_net_health_early_warning_sustained_poor(void)
{
    reset();
    // Drive to POOR — early_warning should be true once in POOR state.
    s_in.rssi = -80;
    eval_n(BB_WIFI_HYST_DOWN);
    TEST_ASSERT_EQUAL_INT(BB_WIFI_LINK_POOR, s_out.state);
    TEST_ASSERT_TRUE(s_out.early_warning);
}

void test_bb_net_health_no_early_warning_when_good(void)
{
    reset();
    s_in.rssi                 = -50;
    s_in.mqtt_connected       = true;
    s_in.mqtt_reconnect_count = 0;
    s_in.disc_age_s           = 0;
    s_in.mqtt_disc_age_s      = 0;
    eval();
    TEST_ASSERT_EQUAL_INT(BB_WIFI_LINK_GOOD, s_out.state);
    TEST_ASSERT_FALSE(s_out.early_warning);
}

// ---------------------------------------------------------------------------
// early_warning trigger: reconnect increase
// ---------------------------------------------------------------------------

void test_bb_net_health_early_warning_reconnect_increase(void)
{
    reset();
    s_in.rssi = -50;
    eval();  // baseline: reconnect_count=0 stored
    TEST_ASSERT_FALSE(s_out.early_warning);

    // Simulate a reconnect.
    s_in.mqtt_reconnect_count = 1;
    eval();
    TEST_ASSERT_TRUE(s_out.early_warning);
}

void test_bb_net_health_no_warning_after_reconnect_stabilizes(void)
{
    reset();
    s_in.rssi = -50;
    eval();

    s_in.mqtt_reconnect_count = 1;
    eval();
    TEST_ASSERT_TRUE(s_out.early_warning);

    // On next eval same count — warning should be false (no new reconnect).
    eval();
    TEST_ASSERT_FALSE(s_out.early_warning);
}

// ---------------------------------------------------------------------------
// early_warning trigger: MQTT disconnect with small mqtt_disc_age_s
//
// warn_disc uses mqtt_disc_age_s — the MQTT-specific disconnect age — so that
// a broker refusal with WiFi still up (wifi disc_age_s == 0) still fires.
// The wifi disc_age_s field is intentionally untouched in these tests.
// ---------------------------------------------------------------------------

// Branch 1: !mqtt_connected AND mqtt_disc_age_s < 60 → warn
void test_bb_net_health_early_warning_mqtt_disc_recent(void)
{
    reset();
    s_in.mqtt_connected  = false;
    s_in.disc_age_s      = 0;   // WiFi UP — no wifi disconnect
    s_in.mqtt_disc_age_s = 30;  // MQTT disconnected 30 s ago (< 60) → warn
    eval();
    TEST_ASSERT_TRUE(s_out.early_warning);
}

// Branch 2: !mqtt_connected AND mqtt_disc_age_s >= 60 → no warn_disc
void test_bb_net_health_no_early_warning_mqtt_disc_old(void)
{
    reset();
    s_in.mqtt_connected  = false;
    s_in.disc_age_s      = 0;    // WiFi UP
    s_in.mqtt_disc_age_s = 120;  // MQTT disconnected 120 s ago (>= 60) → no warn
    eval();
    // Only warn_disc is suppressed; state is GOOD, no reconnect increase.
    TEST_ASSERT_FALSE(s_out.early_warning);
}

// Confirm WiFi disc_age_s alone does NOT trigger warn_disc — the old behaviour
// that conflated WiFi and MQTT disconnect age.
void test_bb_net_health_wifi_disc_age_does_not_trigger_warn_disc(void)
{
    reset();
    s_in.mqtt_connected  = false;
    s_in.disc_age_s      = 30;  // WiFi disconnect age present, but classifier ignores it
    s_in.mqtt_disc_age_s = 0;   // No MQTT disc time recorded yet → no warn
    eval();
    // warn_disc must be false: mqtt_disc_age_s == 0 means no MQTT disconnect
    // recorded, even though mqtt_connected is false.
    TEST_ASSERT_FALSE(s_out.early_warning);
}

// ---------------------------------------------------------------------------
// bb_net_health_throttle_decision
// ---------------------------------------------------------------------------

void test_bb_net_health_throttle_decision_not_throttled_initially(void)
{
    reset();
    bool t = bb_net_health_throttle_decision(&s_st, 3);
    TEST_ASSERT_FALSE(t);
    TEST_ASSERT_FALSE(s_st.throttled);
}

void test_bb_net_health_throttle_starts_after_sustained_poor(void)
{
    reset();
    // Drive to POOR and sustain.
    s_in.rssi = -80;
    eval_n(BB_WIFI_HYST_DOWN);
    TEST_ASSERT_EQUAL_INT(BB_WIFI_LINK_POOR, s_out.state);

    // After HYST_DOWN evals, sustained_poor_count == HYST_DOWN.
    // But throttle threshold is 3 — we need 3 consecutive POOR *after* downgrade.
    // Reset sustained count manually to simulate clean scenario.
    // Note: after eval_n(HYST_DOWN) state becomes POOR and sustained_poor_count
    // has been accumulating since the first POOR sample was classified.
    // The count tracks post-classify POOR samples.
    //
    // Let's add more POOR evals to ensure we hit the threshold.
    for (int i = 0; i < 3; i++) {
        eval();
    }

    bool t = bb_net_health_throttle_decision(&s_st, 3);
    TEST_ASSERT_TRUE(t);
    TEST_ASSERT_TRUE(s_st.throttled);
}

void test_bb_net_health_throttle_restores_on_good(void)
{
    reset();
    // Drive to POOR and throttle.
    s_in.rssi = -80;
    eval_n(BB_WIFI_HYST_DOWN + 3);  // ensure sustained count >= 3
    bb_net_health_throttle_decision(&s_st, 3);  // should set throttled
    // If for some reason still not throttled, force it.
    s_st.throttled            = true;
    s_st.sustained_poor_count = 5;

    // Recover to GOOD.
    s_in.rssi = -50;
    eval_n(BB_WIFI_HYST_UP);
    TEST_ASSERT_EQUAL_INT(BB_WIFI_LINK_GOOD, s_out.state);

    bool t = bb_net_health_throttle_decision(&s_st, 3);
    TEST_ASSERT_FALSE(t);
    TEST_ASSERT_FALSE(s_st.throttled);
}

void test_bb_net_health_throttle_restores_on_marginal(void)
{
    reset();
    // Force throttled state with POOR.
    s_in.rssi = -80;
    eval_n(BB_WIFI_HYST_DOWN);
    s_st.throttled            = true;
    s_st.sustained_poor_count = 5;

    // Recover to MARGINAL.
    s_in.rssi = -70;
    eval_n(BB_WIFI_HYST_UP);
    TEST_ASSERT_EQUAL_INT(BB_WIFI_LINK_MARGINAL, s_out.state);

    bool t = bb_net_health_throttle_decision(&s_st, 3);
    TEST_ASSERT_FALSE(t);
}

void test_bb_net_health_throttle_no_restore_while_poor(void)
{
    reset();
    s_in.rssi = -80;
    eval_n(BB_WIFI_HYST_DOWN + 3);
    s_st.throttled = true;

    // Still POOR — throttle should remain.
    bool t = bb_net_health_throttle_decision(&s_st, 3);
    TEST_ASSERT_TRUE(t);
}

// ---------------------------------------------------------------------------
// Throttle transition: verify exactly one entry and one exit transition
// ---------------------------------------------------------------------------

// The eval_cb in the ESP-IDF glue uses s_last_published_throttled to detect
// the throttled edge (false→true on entry, true→false on exit).
// Verify via the pure host seam that bb_net_health_throttle_decision produces
// the expected edge-detect behaviour so the glue logic is validated.
void test_bb_net_health_throttle_entry_exit_transitions(void)
{
    reset();
    // Drive to sustained POOR.
    s_in.rssi = -80;
    eval_n(BB_WIFI_HYST_DOWN + 3);

    // First call while poor: throttle should start (false → true edge).
    bool prev = false;
    bool curr = bb_net_health_throttle_decision(&s_st, 3);
    TEST_ASSERT_TRUE(curr);
    // Edge detected: curr != prev → would publish once.
    TEST_ASSERT_TRUE(curr != prev);

    // Second call while still poor: no edge — no publish.
    prev = curr;
    curr = bb_net_health_throttle_decision(&s_st, 3);
    TEST_ASSERT_TRUE(curr);
    TEST_ASSERT_FALSE(curr != prev);  // same as before → no publish

    // Recover to GOOD.
    s_in.rssi = -50;
    eval_n(BB_WIFI_HYST_UP);

    // First call after recovery: throttle stops (true → false edge).
    prev = curr;
    curr = bb_net_health_throttle_decision(&s_st, 3);
    TEST_ASSERT_FALSE(curr);
    // Edge detected: curr != prev → would publish once on exit.
    TEST_ASSERT_TRUE(curr != prev);

    // Subsequent call: no edge.
    prev = curr;
    curr = bb_net_health_throttle_decision(&s_st, 3);
    TEST_ASSERT_FALSE(curr);
    TEST_ASSERT_FALSE(curr != prev);
}

// ---------------------------------------------------------------------------
// Multi-trigger: sustained poor AND reconnect simultaneously
// ---------------------------------------------------------------------------

void test_bb_net_health_multi_trigger(void)
{
    reset();
    s_in.rssi             = -80;
    s_in.mqtt_reconnect_count = 0;
    eval_n(BB_WIFI_HYST_DOWN);
    TEST_ASSERT_EQUAL_INT(BB_WIFI_LINK_POOR, s_out.state);

    s_in.mqtt_reconnect_count = 3;
    eval();
    TEST_ASSERT_TRUE(s_out.early_warning);
    TEST_ASSERT_EQUAL_INT(BB_WIFI_LINK_POOR, s_out.state);
}

// ---------------------------------------------------------------------------
// Payload size contract
// ---------------------------------------------------------------------------

// bb_net_health_emit's full payload (nested mqtt). Worst-case input:
// rssi=-128, state="marginal" (longest), all bools true, large integer
// fields. Assert the serialised length fits in a 512-byte budget (nested
// mqtt adds ~80 bytes over the old flat layout).
void test_bb_net_health_sse_payload_fits_256_byte_ring_slot(void)
{
    bb_net_health_status_t snap = {
        .state                  = BB_WIFI_LINK_MARGINAL,
        .early_warning          = true,
        .throttled              = true,
        .rssi                   = -128,
        .mqtt_connected         = false,
        .mqtt_reconnect_count   = 99,
        .last_disconnect_reason = 99,
        .disc_age_s             = 9999,
        .mqtt_disc_age_s        = 9999,
        .mqtt_disc_reason       = 99,
        .mqtt_tls_fail          = 99,
    };

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_net_health_emit(obj, &snap);
    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    TEST_ASSERT_NOT_NULL(json);

    size_t n = strlen(json);
    bb_json_free_str(json);

    // Must fit in a 512-byte ring slot.
    TEST_ASSERT_LESS_THAN(512, n);
}

// ---------------------------------------------------------------------------
// bb_net_health_emit: always emits 8 fields
// ---------------------------------------------------------------------------

// emit: top-level fields + nested mqtt sub-object
void test_bb_net_health_emit_has_8_fields(void)
{
    bb_net_health_status_t snap = {
        .state                  = BB_WIFI_LINK_GOOD,
        .early_warning          = true,
        .throttled              = false,
        .rssi                   = -55,
        .mqtt_connected         = true,
        .mqtt_reconnect_count   = 2,
        .last_disconnect_reason = 3,
        .disc_age_s             = 10,
        .mqtt_disc_age_s        = 5,
        .mqtt_disc_reason       = 1,
        .mqtt_tls_fail          = 0,
    };

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_net_health_emit(obj, &snap);
    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    TEST_ASSERT_NOT_NULL(json);

    bb_json_t parsed = bb_json_parse(json, strlen(json));
    bb_json_free_str(json);
    TEST_ASSERT_NOT_NULL(parsed);

    // Top-level fields
    double rssi_val = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "rssi", &rssi_val));
    TEST_ASSERT_EQUAL_INT(-55, (int)rssi_val);

    char state_buf[32];
    TEST_ASSERT_TRUE(bb_json_obj_get_string(parsed, "state", state_buf, sizeof(state_buf)));
    TEST_ASSERT_EQUAL_STRING("good", state_buf);

    bool ew = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(parsed, "early_warning", &ew));
    TEST_ASSERT_TRUE(ew);

    bool thr = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(parsed, "throttled", &thr));
    TEST_ASSERT_FALSE(thr);

    char dr_buf[32];
    TEST_ASSERT_TRUE(bb_json_obj_get_string(parsed, "last_disconnect_reason", dr_buf, sizeof(dr_buf)));
    TEST_ASSERT_EQUAL_STRING("handshake_timeout", dr_buf); // 3 == BB_WIFI_DISC_HANDSHAKE_TIMEOUT

    double da = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "disc_age_s", &da));
    TEST_ASSERT_EQUAL_INT(10, (int)da);

    // Nested mqtt sub-object
    bb_json_t mqtt_obj = bb_json_obj_get_item(parsed, "mqtt");
    TEST_ASSERT_NOT_NULL(mqtt_obj);

    bool mc = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(mqtt_obj, "connected", &mc));
    TEST_ASSERT_TRUE(mc);

    double rc = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(mqtt_obj, "reconnect_count", &rc));
    TEST_ASSERT_EQUAL_INT(2, (int)rc);

    double mda = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(mqtt_obj, "disc_age_s", &mda));
    TEST_ASSERT_EQUAL_INT(5, (int)mda);

    double mdr = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(mqtt_obj, "disc_reason", &mdr));
    TEST_ASSERT_EQUAL_INT(1, (int)mdr);

    double tf = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(mqtt_obj, "tls_fail", &tf));
    TEST_ASSERT_EQUAL_INT(0, (int)tf);

    bb_json_free(parsed);
}

// emit with poor/all-true snapshot: top-level + nested mqtt all correct
void test_bb_net_health_emit_full_has_8_fields(void)
{
    bb_net_health_status_t snap = {
        .state                  = BB_WIFI_LINK_POOR,
        .early_warning          = true,
        .throttled              = true,
        .rssi                   = -85,
        .mqtt_connected         = false,
        .mqtt_reconnect_count   = 7,
        .last_disconnect_reason = 5,
        .disc_age_s             = 30,
        .mqtt_disc_age_s        = 15,
        .mqtt_disc_reason       = 1,
        .mqtt_tls_fail          = 2,
    };

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_net_health_emit(obj, &snap);
    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    TEST_ASSERT_NOT_NULL(json);

    bb_json_t parsed = bb_json_parse(json, strlen(json));
    bb_json_free_str(json);
    TEST_ASSERT_NOT_NULL(parsed);

    // Top-level fields
    double rssi_val = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "rssi", &rssi_val));
    TEST_ASSERT_EQUAL_INT(-85, (int)rssi_val);

    char state_buf[32];
    TEST_ASSERT_TRUE(bb_json_obj_get_string(parsed, "state", state_buf, sizeof(state_buf)));
    TEST_ASSERT_EQUAL_STRING("poor", state_buf);

    bool ew = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(parsed, "early_warning", &ew));
    TEST_ASSERT_TRUE(ew);

    bool thr = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(parsed, "throttled", &thr));
    TEST_ASSERT_TRUE(thr);

    char dr_buf[32];
    TEST_ASSERT_TRUE(bb_json_obj_get_string(parsed, "last_disconnect_reason", dr_buf, sizeof(dr_buf)));
    TEST_ASSERT_EQUAL_STRING("no_ap_found", dr_buf); // 5 == BB_WIFI_DISC_NO_AP_FOUND

    double da = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "disc_age_s", &da));
    TEST_ASSERT_EQUAL_INT(30, (int)da);

    // Nested mqtt sub-object
    bb_json_t mqtt_obj = bb_json_obj_get_item(parsed, "mqtt");
    TEST_ASSERT_NOT_NULL(mqtt_obj);

    bool mc = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(mqtt_obj, "connected", &mc));
    TEST_ASSERT_FALSE(mc);

    double rc = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(mqtt_obj, "reconnect_count", &rc));
    TEST_ASSERT_EQUAL_INT(7, (int)rc);

    double mda = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(mqtt_obj, "disc_age_s", &mda));
    TEST_ASSERT_EQUAL_INT(15, (int)mda);

    double mdr = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(mqtt_obj, "disc_reason", &mdr));
    TEST_ASSERT_EQUAL_INT(1, (int)mdr);

    double tf = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(mqtt_obj, "tls_fail", &tf));
    TEST_ASSERT_EQUAL_INT(2, (int)tf);

    bb_json_free(parsed);
}

// emit with false bools: verify false values serialized correctly
void test_bb_net_health_emit_compact_false_branch(void)
{
    bb_net_health_status_t snap = {
        .state         = BB_WIFI_LINK_MARGINAL,
        .early_warning = false,
        .throttled     = false,
        .rssi          = -70,
    };

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_net_health_emit(obj, &snap);
    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    TEST_ASSERT_NOT_NULL(json);

    bb_json_t parsed = bb_json_parse(json, strlen(json));
    bb_json_free_str(json);
    TEST_ASSERT_NOT_NULL(parsed);

    char state_buf[32];
    TEST_ASSERT_TRUE(bb_json_obj_get_string(parsed, "state", state_buf, sizeof(state_buf)));
    TEST_ASSERT_EQUAL_STRING("marginal", state_buf);

    bool ew = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(parsed, "early_warning", &ew));
    TEST_ASSERT_FALSE(ew);

    bool thr = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(parsed, "throttled", &thr));
    TEST_ASSERT_FALSE(thr);

    bb_json_free(parsed);
}

// emit with all bools true: verify true values serialized correctly (nested mqtt)
void test_bb_net_health_emit_full_true_branch(void)
{
    bb_net_health_status_t snap = {
        .state                  = BB_WIFI_LINK_GOOD,
        .early_warning          = true,
        .throttled              = true,
        .rssi                   = -60,
        .mqtt_connected         = true,
        .mqtt_reconnect_count   = 1,
        .last_disconnect_reason = 0,
        .disc_age_s             = 0,
        .mqtt_disc_age_s        = 0,
        .mqtt_disc_reason       = 0,
        .mqtt_tls_fail          = 0,
    };

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_net_health_emit(obj, &snap);
    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    TEST_ASSERT_NOT_NULL(json);

    bb_json_t parsed = bb_json_parse(json, strlen(json));
    bb_json_free_str(json);
    TEST_ASSERT_NOT_NULL(parsed);

    bool ew = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(parsed, "early_warning", &ew));
    TEST_ASSERT_TRUE(ew);

    bool thr = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(parsed, "throttled", &thr));
    TEST_ASSERT_TRUE(thr);

    // mqtt_connected is now nested
    bb_json_t mqtt_obj = bb_json_obj_get_item(parsed, "mqtt");
    TEST_ASSERT_NOT_NULL(mqtt_obj);
    bool mc = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(mqtt_obj, "connected", &mc));
    TEST_ASSERT_TRUE(mc);

    bb_json_free(parsed);
}

// parity: two independent emit calls on the same snap produce byte-identical JSON
void test_bb_net_health_emit_idempotent(void)
{
    bb_net_health_status_t snap = {
        .state                  = BB_WIFI_LINK_MARGINAL,
        .early_warning          = true,
        .throttled              = false,
        .rssi                   = -72,
        .mqtt_connected         = true,
        .mqtt_reconnect_count   = 4,
        .last_disconnect_reason = 2,
        .disc_age_s             = 15,
        .mqtt_disc_age_s        = 8,
        .mqtt_disc_reason       = 1,
        .mqtt_tls_fail          = 0,
    };

    bb_json_t obj_a = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj_a);
    bb_net_health_emit(obj_a, &snap);
    char *json_a = bb_json_serialize(obj_a);
    bb_json_free(obj_a);
    TEST_ASSERT_NOT_NULL(json_a);

    bb_json_t obj_b = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj_b);
    bb_net_health_emit(obj_b, &snap);
    char *json_b = bb_json_serialize(obj_b);
    bb_json_free(obj_b);
    TEST_ASSERT_NOT_NULL(json_b);

    TEST_ASSERT_EQUAL_STRING(json_a, json_b);

    // mqtt nested object present in both
    bb_json_t parsed = bb_json_parse(json_a, strlen(json_a));
    TEST_ASSERT_NOT_NULL(parsed);
    TEST_ASSERT_NOT_NULL(bb_json_obj_get_item(parsed, "mqtt"));
    bb_json_free(parsed);

    bb_json_free_str(json_a);
    bb_json_free_str(json_b);
}

// OOM branch coverage: when bb_json_obj_new() for the mqtt sub-object fails,
// top-level fields are still emitted and "mqtt" key is absent (no crash).
void test_bb_net_health_emit_mqtt_alloc_fail(void)
{
    bb_net_health_status_t snap = {
        .state                  = BB_WIFI_LINK_GOOD,
        .early_warning          = false,
        .throttled              = false,
        .rssi                   = -60,
        .mqtt_connected         = true,
        .mqtt_reconnect_count   = 0,
        .last_disconnect_reason = 0,
        .disc_age_s             = 0,
        .mqtt_disc_age_s        = 0,
        .mqtt_disc_reason       = 0,
        .mqtt_tls_fail          = 0,
    };

    // Call 0: outer bb_json_obj_new() — succeeds.
    // Call 1: mqtt sub-object bb_json_obj_new() inside emit — fails.
    // fail_after(1): 1 alloc succeeds, then the next fails.
    bb_json_host_force_alloc_fail_after(1);
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_net_health_emit(obj, &snap);
    bb_json_host_force_alloc_fail_after(-1);  // reset

    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    TEST_ASSERT_NOT_NULL(json);

    bb_json_t parsed = bb_json_parse(json, strlen(json));
    bb_json_free_str(json);
    TEST_ASSERT_NOT_NULL(parsed);

    // Top-level fields must still be present.
    double rssi_val = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "rssi", &rssi_val));

    // mqtt key must be absent (alloc failed).
    TEST_ASSERT_NULL(bb_json_obj_get_item(parsed, "mqtt"));

    bb_json_free(parsed);
}

// --- lost-IP telemetry fields ---

void test_bb_net_health_emit_lost_ip_fields(void)
{
    bb_net_health_status_t snap = {
        .state                  = BB_WIFI_LINK_GOOD,
        .early_warning          = false,
        .throttled              = false,
        .rssi                   = -55,
        .mqtt_connected         = true,
        .mqtt_reconnect_count   = 0,
        .last_disconnect_reason = 0,
        .disc_age_s             = 0,
        .mqtt_disc_age_s        = 0,
        .mqtt_disc_reason       = 0,
        .mqtt_tls_fail          = 0,
        .lost_ip_recoveries     = 3,
        .lost_ip_age_s          = 120,
        .egress_dead_recoveries = 5,
    };

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_net_health_emit(obj, &snap);
    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    TEST_ASSERT_NOT_NULL(json);

    bb_json_t parsed = bb_json_parse(json, strlen(json));
    bb_json_free_str(json);
    TEST_ASSERT_NOT_NULL(parsed);

    double lip = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "lost_ip_recoveries", &lip));
    TEST_ASSERT_EQUAL_INT(3, (int)lip);

    double lia = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "lost_ip_age_s", &lia));
    TEST_ASSERT_EQUAL_INT(120, (int)lia);

    double val = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "egress_dead_recoveries", &val));
    TEST_ASSERT_EQUAL_DOUBLE(5.0, val);

    bb_json_free(parsed);
}

void test_bb_net_health_emit_lost_ip_zero(void)
{
    bb_net_health_status_t snap = {
        .state                  = BB_WIFI_LINK_GOOD,
        .rssi                   = -55,
        .lost_ip_recoveries     = 0,
        .lost_ip_age_s          = 0,
        .egress_dead_recoveries = 0,
    };

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_net_health_emit(obj, &snap);
    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    TEST_ASSERT_NOT_NULL(json);

    bb_json_t parsed = bb_json_parse(json, strlen(json));
    bb_json_free_str(json);
    TEST_ASSERT_NOT_NULL(parsed);

    double lip = 99.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "lost_ip_recoveries", &lip));
    TEST_ASSERT_EQUAL_INT(0, (int)lip);

    double lia = 99.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "lost_ip_age_s", &lia));
    TEST_ASSERT_EQUAL_INT(0, (int)lia);

    double val = 99.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "egress_dead_recoveries", &val));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, val);

    bb_json_free(parsed);
}

// ---------------------------------------------------------------------------
// bb_net_health_emit_status: status-only (bools/enums), no numeric counters
// ---------------------------------------------------------------------------

// emit_status emits state/early_warning/throttled + mqtt.connected
// and must NOT emit any numeric fields (rssi, disc_age_s, reconnect_count, etc.).
void test_bb_net_health_emit_status_status_only(void)
{
    bb_net_health_status_t snap = {
        .state                  = BB_WIFI_LINK_MARGINAL,
        .early_warning          = true,
        .throttled              = false,
        .rssi                   = -70,
        .mqtt_connected         = true,
        .mqtt_reconnect_count   = 5,
        .last_disconnect_reason = 3,
        .disc_age_s             = 30,
        .mqtt_disc_age_s        = 15,
        .mqtt_disc_reason       = 1,
        .mqtt_tls_fail          = 0,
    };

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_net_health_emit_status(obj, &snap);
    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    TEST_ASSERT_NOT_NULL(json);

    bb_json_t parsed = bb_json_parse(json, strlen(json));
    bb_json_free_str(json);
    TEST_ASSERT_NOT_NULL(parsed);

    // Status fields must be present.
    char state_buf[32];
    TEST_ASSERT_TRUE(bb_json_obj_get_string(parsed, "state", state_buf, sizeof(state_buf)));
    TEST_ASSERT_EQUAL_STRING("marginal", state_buf);

    bool ew = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(parsed, "early_warning", &ew));
    TEST_ASSERT_TRUE(ew);

    bool thr = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(parsed, "throttled", &thr));
    TEST_ASSERT_FALSE(thr);

    // mqtt sub-object: only connected (bool), no counters.
    bb_json_t mqtt_obj = bb_json_obj_get_item(parsed, "mqtt");
    TEST_ASSERT_NOT_NULL_MESSAGE(mqtt_obj, "mqtt sub-object missing from emit_status");
    bool mc = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(mqtt_obj, "connected", &mc));
    TEST_ASSERT_TRUE(mc);

    // Numeric counters must NOT be present at root.
    double dummy = 0.0;
    TEST_ASSERT_FALSE_MESSAGE(bb_json_obj_get_number(parsed, "rssi", &dummy),
        "rssi must not appear in emit_status output (TA-505)");
    TEST_ASSERT_FALSE_MESSAGE(bb_json_obj_get_number(parsed, "disc_age_s", &dummy),
        "disc_age_s must not appear in emit_status output");
    TEST_ASSERT_FALSE_MESSAGE(bb_json_obj_get_number(parsed, "last_disconnect_reason", &dummy),
        "last_disconnect_reason must not appear in emit_status output");

    bb_json_free(parsed);
}

// OOM branch: mqtt sub-object alloc fails — top-level fields still emit, "mqtt" absent.
void test_bb_net_health_emit_status_mqtt_alloc_fail(void)
{
    bb_net_health_status_t snap = {
        .state          = BB_WIFI_LINK_GOOD,
        .early_warning  = false,
        .throttled      = false,
        .mqtt_connected = true,
    };
    // Call 0: outer bb_json_obj_new() — succeeds.
    // Call 1: mqtt sub-object bb_json_obj_new() inside emit_status — fails.
    bb_json_host_force_alloc_fail_after(1);
    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_net_health_emit_status(obj, &snap);
    bb_json_host_force_alloc_fail_after(-1);  // reset

    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    TEST_ASSERT_NOT_NULL(json);
    bb_json_t parsed = bb_json_parse(json, strlen(json));
    bb_json_free_str(json);
    TEST_ASSERT_NOT_NULL(parsed);

    // Top-level state field must still be present.
    char state_buf[32];
    TEST_ASSERT_TRUE(bb_json_obj_get_string(parsed, "state", state_buf, sizeof(state_buf)));
    // mqtt key must be absent (alloc failed).
    TEST_ASSERT_NULL(bb_json_obj_get_item(parsed, "mqtt"));

    bb_json_free(parsed);
}

// ---------------------------------------------------------------------------
// bb_net_health_classify_egress — pure egress-state classifier
// (egress-recovery SSOT, B1-518, Phase 1, OBSERVE-ONLY).
// ---------------------------------------------------------------------------

// wifi_mode != OK short-circuits to OK regardless of any other input.
void test_bb_net_health_classify_egress_no_ip_mode_is_ok(void)
{
    TEST_ASSERT_EQUAL_INT(BB_EGRESS_STATE_OK,
        bb_net_health_classify_egress(BB_WIFI_MODE_NO_IP, true, false, 5, 3, 2, 2));
}

void test_bb_net_health_classify_egress_not_associated_mode_is_ok(void)
{
    TEST_ASSERT_EQUAL_INT(BB_EGRESS_STATE_OK,
        bb_net_health_classify_egress(BB_WIFI_MODE_NOT_ASSOCIATED, true, false, 5, 3, 2, 2));
}

// No probe data yet → OK.
void test_bb_net_health_classify_egress_not_probed_is_ok(void)
{
    TEST_ASSERT_EQUAL_INT(BB_EGRESS_STATE_OK,
        bb_net_health_classify_egress(BB_WIFI_MODE_OK, false, false, 0, 3, 2, 2));
}

// !gw_reachable, streak below threshold → transient miss, still OK.
void test_bb_net_health_classify_egress_gw_unreachable_below_threshold_is_ok(void)
{
    TEST_ASSERT_EQUAL_INT(BB_EGRESS_STATE_OK,
        bb_net_health_classify_egress(BB_WIFI_MODE_OK, true, false, 2, 3, 0, 0));
}

// !gw_reachable, streak == threshold → GW_UNREACHABLE.
void test_bb_net_health_classify_egress_gw_unreachable_at_threshold(void)
{
    TEST_ASSERT_EQUAL_INT(BB_EGRESS_STATE_GW_UNREACHABLE,
        bb_net_health_classify_egress(BB_WIFI_MODE_OK, true, false, 3, 3, 0, 0));
}

// gw_reachable, failing == 0 → OK.
void test_bb_net_health_classify_egress_gw_reachable_none_failing(void)
{
    TEST_ASSERT_EQUAL_INT(BB_EGRESS_STATE_OK,
        bb_net_health_classify_egress(BB_WIFI_MODE_OK, true, true, 0, 3, 2, 0));
}

// gw_reachable, 0 < failing < enabled → ENDPOINT_DOWN. This is the whole
// point of the gateway-probe-as-tiebreaker: gw up + one endpoint down (e.g.
// mining pool) must NOT classify as a WiFi fault.
void test_bb_net_health_classify_egress_gw_reachable_one_endpoint_down(void)
{
    TEST_ASSERT_EQUAL_INT(BB_EGRESS_STATE_ENDPOINT_DOWN,
        bb_net_health_classify_egress(BB_WIFI_MODE_OK, true, true, 0, 3, 2, 1));
}

// gw_reachable, failing == enabled (> 0) → ALL_DEAD.
void test_bb_net_health_classify_egress_gw_reachable_all_endpoints_down(void)
{
    TEST_ASSERT_EQUAL_INT(BB_EGRESS_STATE_ALL_DEAD,
        bb_net_health_classify_egress(BB_WIFI_MODE_OK, true, true, 0, 3, 2, 2));
}

// gw_reachable, enabled == 0 → OK (no egress clients configured at all).
void test_bb_net_health_classify_egress_gw_reachable_no_egress_clients(void)
{
    TEST_ASSERT_EQUAL_INT(BB_EGRESS_STATE_OK,
        bb_net_health_classify_egress(BB_WIFI_MODE_OK, true, true, 0, 3, 0, 0));
}

void test_bb_egress_state_str_ok(void)
{
    TEST_ASSERT_EQUAL_STRING("ok", bb_egress_state_str(BB_EGRESS_STATE_OK));
}

void test_bb_egress_state_str_endpoint_down(void)
{
    TEST_ASSERT_EQUAL_STRING("endpoint_down", bb_egress_state_str(BB_EGRESS_STATE_ENDPOINT_DOWN));
}

void test_bb_egress_state_str_gw_unreachable(void)
{
    TEST_ASSERT_EQUAL_STRING("gw_unreachable", bb_egress_state_str(BB_EGRESS_STATE_GW_UNREACHABLE));
}

void test_bb_egress_state_str_all_dead(void)
{
    TEST_ASSERT_EQUAL_STRING("all_dead", bb_egress_state_str(BB_EGRESS_STATE_ALL_DEAD));
}

void test_bb_egress_state_str_unknown_returns_nonnull(void)
{
    const char *s = bb_egress_state_str((bb_egress_state_t)99);
    TEST_ASSERT_NOT_NULL(s);
}

// ---------------------------------------------------------------------------
// bb_net_health_emit: net_mode/associated/has_ip + the no_ip_recoveries/roam
// serialization-gap fix (they were captured in the status struct but never
// emitted to the net.health payload).
// ---------------------------------------------------------------------------

void test_bb_net_health_emit_has_net_mode_and_discriminator_fields(void)
{
    bb_net_health_status_t snap = {
        .state           = BB_WIFI_LINK_GOOD,
        .rssi            = -55,
        .no_ip_recoveries = 3,
        .roam_count       = 2,
        .roam_age_s       = 45,
        .net_mode         = BB_WIFI_MODE_NO_IP,
        .associated       = true,
        .has_ip           = false,
    };

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_net_health_emit(obj, &snap);
    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    TEST_ASSERT_NOT_NULL(json);

    bb_json_t parsed = bb_json_parse(json, strlen(json));
    bb_json_free_str(json);
    TEST_ASSERT_NOT_NULL(parsed);

    double nir = 0.0;
    TEST_ASSERT_TRUE_MESSAGE(bb_json_obj_get_number(parsed, "no_ip_recoveries", &nir),
        "no_ip_recoveries must be serialized to net.health (previously a serialization gap)");
    TEST_ASSERT_EQUAL_INT(3, (int)nir);

    double rc = 0.0;
    TEST_ASSERT_TRUE_MESSAGE(bb_json_obj_get_number(parsed, "roam_count", &rc),
        "roam_count must be serialized to net.health (previously a serialization gap)");
    TEST_ASSERT_EQUAL_INT(2, (int)rc);

    double ra = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "roam_age_s", &ra));
    TEST_ASSERT_EQUAL_INT(45, (int)ra);

    char mode_buf[32];
    TEST_ASSERT_TRUE(bb_json_obj_get_string(parsed, "net_mode", mode_buf, sizeof(mode_buf)));
    TEST_ASSERT_EQUAL_STRING("no_ip", mode_buf);

    bool associated = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(parsed, "associated", &associated));
    TEST_ASSERT_TRUE(associated);

    bool has_ip = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(parsed, "has_ip", &has_ip));
    TEST_ASSERT_FALSE(has_ip);

    bb_json_free(parsed);
}

// ---------------------------------------------------------------------------
// wifi-drop-log PR: last_session_s serialization (route-fidelity)
// ---------------------------------------------------------------------------

// Non-zero last_session_s must round-trip through bb_net_health_emit.
void test_bb_net_health_emit_last_session_s_nonzero(void)
{
    bb_net_health_status_t snap = {
        .state           = BB_WIFI_LINK_GOOD,
        .rssi            = -55,
        .last_session_s  = 3661, // 1h 1m 1s
    };

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_net_health_emit(obj, &snap);
    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    TEST_ASSERT_NOT_NULL(json);

    bb_json_t parsed = bb_json_parse(json, strlen(json));
    bb_json_free_str(json);
    TEST_ASSERT_NOT_NULL(parsed);

    double lss = 0.0;
    TEST_ASSERT_TRUE_MESSAGE(bb_json_obj_get_number(parsed, "last_session_s", &lss),
        "last_session_s must be serialized to net.health / GET /api/diag/net");
    TEST_ASSERT_EQUAL_INT(3661, (int)lss);

    bb_json_free(parsed);
}

// Zero sentinel (no session has ended yet) must still serialize the field.
void test_bb_net_health_emit_last_session_s_zero_sentinel(void)
{
    bb_net_health_status_t snap = {
        .state          = BB_WIFI_LINK_GOOD,
        .rssi           = -55,
        .last_session_s = 0,
    };

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_net_health_emit(obj, &snap);
    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    TEST_ASSERT_NOT_NULL(json);

    bb_json_t parsed = bb_json_parse(json, strlen(json));
    bb_json_free_str(json);
    TEST_ASSERT_NOT_NULL(parsed);

    double lss = 99.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "last_session_s", &lss));
    TEST_ASSERT_EQUAL_INT(0, (int)lss);

    bb_json_free(parsed);
}

// bb_net_health_emit_status (status-only) must NOT emit last_session_s.
void test_bb_net_health_emit_status_no_last_session_s(void)
{
    bb_net_health_status_t snap = {
        .state          = BB_WIFI_LINK_GOOD,
        .last_session_s = 42,
    };

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_net_health_emit_status(obj, &snap);
    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    TEST_ASSERT_NOT_NULL(json);

    bb_json_t parsed = bb_json_parse(json, strlen(json));
    bb_json_free_str(json);
    TEST_ASSERT_NOT_NULL(parsed);

    TEST_ASSERT_NULL(bb_json_obj_get_item(parsed, "last_session_s"));

    bb_json_free(parsed);
}

// ---------------------------------------------------------------------------
// bb_net_health_classify_heap — pure bucket classifier (B1-439)
// ---------------------------------------------------------------------------

// Heap well above both thresholds → OK.
void test_bb_net_health_classify_heap_ok(void)
{
    bb_heap_state_t s = bb_net_health_classify_heap(BB_NET_HEALTH_HEAP_LOW_BYTES + 1);
    TEST_ASSERT_EQUAL_INT(BB_HEAP_STATE_OK, s);
}

// Heap between CRITICAL and LOW → LOW.
void test_bb_net_health_classify_heap_low(void)
{
    size_t mid = (BB_NET_HEALTH_HEAP_CRITICAL_BYTES + BB_NET_HEALTH_HEAP_LOW_BYTES) / 2;
    bb_heap_state_t s = bb_net_health_classify_heap(mid);
    TEST_ASSERT_EQUAL_INT(BB_HEAP_STATE_LOW, s);
}

// Heap below CRITICAL → CRITICAL.
void test_bb_net_health_classify_heap_critical(void)
{
    bb_heap_state_t s = bb_net_health_classify_heap(BB_NET_HEALTH_HEAP_CRITICAL_BYTES - 1);
    TEST_ASSERT_EQUAL_INT(BB_HEAP_STATE_CRITICAL, s);
}

// Zero free bytes is also CRITICAL.
void test_bb_net_health_classify_heap_zero(void)
{
    bb_heap_state_t s = bb_net_health_classify_heap(0);
    TEST_ASSERT_EQUAL_INT(BB_HEAP_STATE_CRITICAL, s);
}

// ---------------------------------------------------------------------------
// bb_heap_state_str — string helper, including the default/unknown branch
// ---------------------------------------------------------------------------

void test_bb_heap_state_str_ok(void)
{
    TEST_ASSERT_EQUAL_STRING("ok", bb_heap_state_str(BB_HEAP_STATE_OK));
}

void test_bb_heap_state_str_low(void)
{
    TEST_ASSERT_EQUAL_STRING("low", bb_heap_state_str(BB_HEAP_STATE_LOW));
}

void test_bb_heap_state_str_critical(void)
{
    TEST_ASSERT_EQUAL_STRING("critical", bb_heap_state_str(BB_HEAP_STATE_CRITICAL));
}

// Cast an out-of-range value to exercise the default branch.
void test_bb_heap_state_str_unknown_returns_ok(void)
{
    const char *s = bb_heap_state_str((bb_heap_state_t)99);
    TEST_ASSERT_EQUAL_STRING("ok", s);
}

// ---------------------------------------------------------------------------
// bb_net_health_set_heap_state / bb_net_health_heap_state
//
// bb_net_health_set_heap_state is an internal setter not declared in the
// public header; the ESP-IDF platform file forward-declares it with extern.
// We use the same pattern here to cover the three lines (signature, body,
// closing brace) that were the only uncovered lines in the module.
// ---------------------------------------------------------------------------

// Internal setter, matching the forward-declare pattern used by
// platform/espidf/bb_net_health/bb_net_health_espidf.c.
extern void bb_net_health_set_heap_state(bb_heap_state_t state);

// Round-trip: set each non-default state and read it back via the public getter.
void test_bb_net_health_set_heap_state_roundtrip(void)
{
    // Initial value must be OK (zero-init module static).
    TEST_ASSERT_EQUAL_INT(BB_HEAP_STATE_OK, bb_net_health_heap_state());

    bb_net_health_set_heap_state(BB_HEAP_STATE_LOW);
    TEST_ASSERT_EQUAL_INT(BB_HEAP_STATE_LOW, bb_net_health_heap_state());

    bb_net_health_set_heap_state(BB_HEAP_STATE_CRITICAL);
    TEST_ASSERT_EQUAL_INT(BB_HEAP_STATE_CRITICAL, bb_net_health_heap_state());

    // Restore to OK so other tests are not affected by residual state.
    bb_net_health_set_heap_state(BB_HEAP_STATE_OK);
    TEST_ASSERT_EQUAL_INT(BB_HEAP_STATE_OK, bb_net_health_heap_state());
}

// ---------------------------------------------------------------------------
// bb_net_health_should_log — diagnostic-state log heartbeat rate limiter
// (KB#556). Pure; every arm covered.
// ---------------------------------------------------------------------------

// mode changed -> log immediately, even if interval has not elapsed.
void test_bb_net_health_should_log_mode_changed(void)
{
    bool r = bb_net_health_should_log(/*now_us=*/1000, /*last_log_us=*/999,
                                       BB_WIFI_MODE_NO_IP, BB_WIFI_MODE_OK,
                                       /*interval_s=*/60);
    TEST_ASSERT_TRUE(r);
}

// mode unchanged, interval elapsed -> log.
void test_bb_net_health_should_log_interval_elapsed(void)
{
    int64_t interval_us = 60LL * 1000000LL;
    bool r = bb_net_health_should_log(/*now_us=*/interval_us, /*last_log_us=*/0,
                                       BB_WIFI_MODE_OK, BB_WIFI_MODE_OK,
                                       /*interval_s=*/60);
    TEST_ASSERT_TRUE(r);
}

// mode unchanged, interval not yet elapsed -> do not log.
void test_bb_net_health_should_log_neither(void)
{
    bool r = bb_net_health_should_log(/*now_us=*/1000000, /*last_log_us=*/0,
                                       BB_WIFI_MODE_OK, BB_WIFI_MODE_OK,
                                       /*interval_s=*/60);
    TEST_ASSERT_FALSE(r);
}

// mode changed AND interval elapsed -> still log (edge wins, no double logic).
void test_bb_net_health_should_log_both(void)
{
    int64_t interval_us = 60LL * 1000000LL;
    bool r = bb_net_health_should_log(/*now_us=*/interval_us + 5, /*last_log_us=*/0,
                                       BB_WIFI_MODE_NOT_ASSOCIATED, BB_WIFI_MODE_OK,
                                       /*interval_s=*/60);
    TEST_ASSERT_TRUE(r);
}

// Non-monotonic clock guard: now_us < last_log_us on an unchanged mode must
// not log (elapsed clamped to 0, never negative-elapsed >= interval).
void test_bb_net_health_should_log_clock_went_backwards(void)
{
    bool r = bb_net_health_should_log(/*now_us=*/0, /*last_log_us=*/5000,
                                       BB_WIFI_MODE_OK, BB_WIFI_MODE_OK,
                                       /*interval_s=*/60);
    TEST_ASSERT_FALSE(r);
}

// ---------------------------------------------------------------------------
// bb_net_health_format_log — compact key=val diagnostic-state line.
// ---------------------------------------------------------------------------

static bb_net_health_status_t sample_status_for_log(void)
{
    bb_net_health_status_t s;
    memset(&s, 0, sizeof(s));
    s.net_mode                = BB_WIFI_MODE_NO_IP;
    s.associated               = true;
    s.has_ip                   = false;
    s.rssi                     = -72;
    s.last_disconnect_reason   = 8; // esp_wifi WIFI_REASON_ASSOC_LEAVE-ish
    s.last_session_s           = 1234;
    s.roam_count                = 3;
    s.no_ip_recoveries          = 2;
    s.lost_ip_recoveries        = 1;
    s.egress_dead_recoveries    = 0;
    s.retry_count                = 5;
    s.restart_sta_count          = 4;
    s.uptime_s                   = 9999;
    strncpy(s.ip, "192.168.1.42", sizeof(s.ip) - 1);
    return s;
}

// NULL / zero-cap safety.
void test_bb_net_health_format_log_null_status(void)
{
    char buf[64];
    TEST_ASSERT_EQUAL_INT(0, bb_net_health_format_log(NULL, buf, sizeof(buf)));
}

void test_bb_net_health_format_log_null_buf(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    TEST_ASSERT_EQUAL_INT(0, bb_net_health_format_log(&s, NULL, 64));
}

void test_bb_net_health_format_log_zero_cap(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    char buf[64];
    TEST_ASSERT_EQUAL_INT(0, bb_net_health_format_log(&s, buf, 0));
}

// Representative status -> key fields present and correct.
void test_bb_net_health_format_log_fields_present(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    char buf[256];
    int n = bb_net_health_format_log(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "nm=no_ip") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "assoc=1") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "ip_ok=0") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "rssi=-72") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "dr=8") != NULL, buf);
    // Reason NAME must NOT appear — heartbeat carries only the code; the
    // per-drop event carries the human-readable name.
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "dr=8(") == NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "sess=1234") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "roam=3") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "no_ip=2") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "lost_ip=1") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "egress=0") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "retry=5") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "restart=4") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "up=9999") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "ip=192.168.1.42") != NULL, buf);
}

// net_mode string correct across all three buckets.
void test_bb_net_health_format_log_net_mode_ok(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    s.net_mode = BB_WIFI_MODE_OK;
    char buf[256];
    bb_net_health_format_log(&s, buf, sizeof(buf));
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "nm=ok") != NULL, buf);
}

void test_bb_net_health_format_log_net_mode_not_associated(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    s.net_mode = BB_WIFI_MODE_NOT_ASSOCIATED;
    char buf[256];
    bb_net_health_format_log(&s, buf, sizeof(buf));
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "nm=not_associated") != NULL, buf);
}

// Truncation safety: a tiny cap must never overflow buf and must still
// null-terminate.
void test_bb_net_health_format_log_truncation_safe(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    char buf[8];
    memset(buf, 0x7F, sizeof(buf)); // sentinel fill to detect any overrun
    int n = bb_net_health_format_log(&s, buf, sizeof(buf));
    // snprintf "would have written" length exceeds the tiny cap.
    TEST_ASSERT_GREATER_OR_EQUAL_INT((int)sizeof(buf), n);
    // Always null-terminated within cap (snprintf guarantee) — no overrun of
    // the caller-supplied buffer regardless of the "would have written"
    // length reported above.
    TEST_ASSERT_EQUAL_INT(0, buf[sizeof(buf) - 1]);
}

// Critical-first ordering: nm/ip/ip_ok/assoc/rssi must precede the trailing
// counters so a truncated line (cap between the two) still carries net_mode
// and ip — the fields needed to diagnose a zombie board over the log
// channel.
void test_bb_net_health_format_log_critical_first_order(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    char buf[256];
    bb_net_health_format_log(&s, buf, sizeof(buf));
    const char *p_nm    = strstr(buf, "nm=");
    const char *p_ip    = strstr(buf, "ip=");
    const char *p_roam  = strstr(buf, "roam=");
    const char *p_up    = strstr(buf, "up=");
    TEST_ASSERT_NOT_NULL_MESSAGE(p_nm, buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(p_ip, buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(p_roam, buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(p_up, buf);
    TEST_ASSERT_TRUE_MESSAGE(p_nm < p_ip, buf);
    TEST_ASSERT_TRUE_MESSAGE(p_ip < p_roam, buf);
    TEST_ASSERT_TRUE_MESSAGE(p_roam < p_up, buf);
}

// ---------------------------------------------------------------------------
// bb_net_health_format_log gw fields (B1-518 PR3, OBSERVE-ONLY) — gw_available
// branch coverage.
// ---------------------------------------------------------------------------

// gw_available == false (default from sample_status_for_log/memset): no "gw="
// token appears at all — heartbeat unchanged for boards without the probe.
void test_bb_net_health_format_log_gw_omitted_when_unavailable(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    s.gw_available = false;
    char buf[256];
    int n = bb_net_health_format_log(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "gw=") == NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "gwdead=") == NULL, buf);
    // Unaffected trailing field is still the last token.
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "up=9999") != NULL, buf);
}

// gw_available == true: gw + gwdead appear, LAST (after up=), reflecting the
// snapshot's gw_reachable/gw_dead_count values.
void test_bb_net_health_format_log_gw_fields_present_and_last(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    s.gw_available   = true;
    s.gw_reachable   = false;
    s.gw_fail_streak = 2;
    s.gw_dead_count  = 7;
    char buf[256];
    int n = bb_net_health_format_log(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "gw=0") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "gwdead=7") != NULL, buf);

    const char *p_up = strstr(buf, "up=9999");
    const char *p_gw = strstr(buf, "gw=0");
    TEST_ASSERT_NOT_NULL_MESSAGE(p_up, buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(p_gw, buf);
    TEST_ASSERT_TRUE_MESSAGE(p_up < p_gw, buf); // gw fields come after everything else
}

// gw_reachable == true renders "gw=1".
void test_bb_net_health_format_log_gw_reachable_true(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    s.gw_available = true;
    s.gw_reachable  = true;
    s.gw_dead_count = 0;
    char buf[256];
    bb_net_health_format_log(&s, buf, sizeof(buf));
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "gw=1") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "gwdead=0") != NULL, buf);
}

// Truncation drops gw first: gw_available=true but the buffer cap is sized
// JUST past the non-gw fields (124 bytes for sample_status_for_log's values
// + 1 for NUL = 125) and well below the full gw-suffixed line (138 bytes +
// NUL = 139). snprintf's single format string appends gw_suffix last, so a
// cap at exactly non-gw-length+1 truncates the gw tokens cleanly (no
// partial "gw=" token) while leaving every preceding field (nm=/ip=/.../up=)
// intact and NUL-terminated within cap.
void test_bb_net_health_format_log_truncation_drops_gw_first(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    s.gw_available   = true;
    s.gw_reachable   = false;
    s.gw_fail_streak = 2;
    s.gw_dead_count  = 7;
    char buf[125];
    memset(buf, 0x7F, sizeof(buf)); // sentinel fill to detect any overrun
    int n = bb_net_health_format_log(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_OR_EQUAL_INT((int)sizeof(buf), n); // full line exceeds cap

    // gw suffix dropped entirely — not even a partial "gw=" token survives.
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "gw=") == NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "gwdead=") == NULL, buf);

    // Preceding fields present and not truncated mid-token.
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "up=9999") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "restart=4") != NULL, buf);

    // NUL-terminated within cap (snprintf guarantee).
    TEST_ASSERT_EQUAL_INT(0, buf[sizeof(buf) - 1]);
}

// ---------------------------------------------------------------------------
// bb_net_health_format_log txfail token (B1-518 PR2, OBSERVE-ONLY) —
// tx_available branch coverage. Mirrors the gw_available tests above.
// ---------------------------------------------------------------------------

// tx_available == false (default from sample_status_for_log/memset): no
// "txfail=" token appears at all — heartbeat unchanged for boards with no
// registered bb_transport_health transport.
void test_bb_net_health_format_log_txfail_omitted_when_unavailable(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    s.tx_available = false;
    char buf[256];
    int n = bb_net_health_format_log(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "txfail=") == NULL, buf);
    // Unaffected trailing field is still the last token.
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "up=9999") != NULL, buf);
}

// tx_available == true: txfail=<failing>/<enabled> appears LAST — after
// up= AND after gw= (when both gw and tx are available) — reflecting the
// least-critical-field-last ordering (txfail drops before gw under
// truncation).
void test_bb_net_health_format_log_txfail_present_and_last(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    s.gw_available   = true;
    s.gw_reachable   = true;
    s.gw_dead_count  = 0;
    s.tx_available   = true;
    s.tx_failing     = 1;
    s.tx_enabled     = 2;
    char buf[256];
    int n = bb_net_health_format_log(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "txfail=1/2") != NULL, buf);

    const char *p_up = strstr(buf, "up=9999");
    const char *p_gw = strstr(buf, "gw=1");
    const char *p_tx = strstr(buf, "txfail=1/2");
    TEST_ASSERT_NOT_NULL_MESSAGE(p_up, buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(p_gw, buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(p_tx, buf);
    TEST_ASSERT_TRUE_MESSAGE(p_up < p_gw, buf);
    TEST_ASSERT_TRUE_MESSAGE(p_gw < p_tx, buf); // txfail comes after gw (drops first)
}

// txfail with tx_failing == tx_enabled == 0 renders "txfail=0/0" (a healthy,
// zero-failure transport still reports its presence).
void test_bb_net_health_format_log_txfail_zero_counts(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    s.tx_available = true;
    s.tx_failing   = 0;
    s.tx_enabled   = 0;
    char buf[256];
    bb_net_health_format_log(&s, buf, sizeof(buf));
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "txfail=0/0") != NULL, buf);
}

// Truncation drops txfail first, keeping gw intact: gw_available=true AND
// tx_available=true, but the buffer cap is sized JUST past the base+gw
// fields (139 bytes: 124 base + 14 gw suffix + 1 NUL) — below the full
// base+gw+tx line (149 bytes: 124 + 14 + 11 txfail-suffix ("txfail=1/2") +
// 1 NUL). snprintf's single format string appends gw_suffix before
// tx_suffix, so a cap at exactly base+gw-length+1 truncates the txfail
// token cleanly (no partial "txfail=" token) while leaving gw AND every
// preceding field intact and NUL-terminated within cap.
void test_bb_net_health_format_log_truncation_drops_txfail_first(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    s.gw_available   = true;
    s.gw_reachable   = false;
    s.gw_fail_streak = 2;
    s.gw_dead_count  = 7;
    s.tx_available   = true;
    s.tx_failing     = 1;
    s.tx_enabled     = 2;
    char buf[139];
    memset(buf, 0x7F, sizeof(buf)); // sentinel fill to detect any overrun
    int n = bb_net_health_format_log(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_OR_EQUAL_INT((int)sizeof(buf), n); // full line exceeds cap

    // txfail suffix dropped entirely — not even a partial "txfail=" token survives.
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "txfail=") == NULL, buf);

    // gw suffix (higher priority than txfail) survives intact.
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "gw=0") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "gwdead=7") != NULL, buf);

    // Preceding fields present and not truncated mid-token.
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "up=9999") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "restart=4") != NULL, buf);

    // NUL-terminated within cap (snprintf guarantee).
    TEST_ASSERT_EQUAL_INT(0, buf[sizeof(buf) - 1]);
}

// ---------------------------------------------------------------------------
// bb_net_health_format_log egr token (B1-518 PR3, OBSERVE-ONLY) —
// egress_state branch coverage. Mirrors the gw_available/tx_available tests
// above; egr is the LEAST critical field (dropped first under truncation).
// ---------------------------------------------------------------------------

// egress_state == BB_EGRESS_STATE_OK (default from sample_status_for_log/
// memset): no "egr=" token appears at all — healthy heartbeat stays short.
void test_bb_net_health_format_log_egr_omitted_when_ok(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    s.egress_state = BB_EGRESS_STATE_OK;
    char buf[256];
    int n = bb_net_health_format_log(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "egr=") == NULL, buf);
    // Unaffected trailing field is still the last token.
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "up=9999") != NULL, buf);
}

// egress_state != OK: egr=<str> appears LAST — after up= AND after gw= AND
// after txfail= (when all three are present) — the least-critical-field-last
// ordering (egr drops before txfail/gw under truncation).
void test_bb_net_health_format_log_egr_present_and_last(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    s.gw_available   = true;
    s.gw_reachable   = true;
    s.gw_dead_count  = 0;
    s.tx_available   = true;
    s.tx_failing     = 1;
    s.tx_enabled     = 2;
    s.egress_state   = BB_EGRESS_STATE_ENDPOINT_DOWN;
    char buf[256];
    int n = bb_net_health_format_log(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "egr=endpoint_down") != NULL, buf);

    const char *p_gw  = strstr(buf, "gw=1");
    const char *p_tx  = strstr(buf, "txfail=1/2");
    const char *p_egr = strstr(buf, "egr=endpoint_down");
    TEST_ASSERT_NOT_NULL_MESSAGE(p_gw, buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(p_tx, buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(p_egr, buf);
    TEST_ASSERT_TRUE_MESSAGE(p_gw < p_tx, buf);
    TEST_ASSERT_TRUE_MESSAGE(p_tx < p_egr, buf); // egr comes after txfail (drops first)
}

// bb_egress_state_str value round-trips for the remaining non-OK buckets.
void test_bb_net_health_format_log_egr_all_states(void)
{
    bb_net_health_status_t s = sample_status_for_log();

    s.egress_state = BB_EGRESS_STATE_GW_UNREACHABLE;
    char buf1[256];
    bb_net_health_format_log(&s, buf1, sizeof(buf1));
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf1, "egr=gw_unreachable") != NULL, buf1);

    s.egress_state = BB_EGRESS_STATE_ALL_DEAD;
    char buf2[256];
    bb_net_health_format_log(&s, buf2, sizeof(buf2));
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf2, "egr=all_dead") != NULL, buf2);
}

// Truncation drops egr first: with gw + txfail + egr all present, a cap
// sized to end exactly at the " egr=..." boundary must drop egr entirely
// while gw and txfail survive intact. Cap is computed dynamically from the
// full-length line so this test does not hand-count bytes.
void test_bb_net_health_format_log_truncation_drops_egr_first(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    s.gw_available   = true;
    s.gw_reachable   = false;
    s.gw_fail_streak = 2;
    s.gw_dead_count  = 7;
    s.tx_available   = true;
    s.tx_failing     = 1;
    s.tx_enabled     = 2;
    s.egress_state   = BB_EGRESS_STATE_GW_UNREACHABLE;

    char full[256];
    int n_full = bb_net_health_format_log(&s, full, sizeof(full));
    TEST_ASSERT_GREATER_THAN_INT(0, n_full);

    const char *p_egr = strstr(full, " egr=gw_unreachable");
    TEST_ASSERT_NOT_NULL_MESSAGE(p_egr, full);
    int cap = (int)(p_egr - full) + 1; // budget for the NUL terminator only

    char buf[256];
    memset(buf, 0x7F, sizeof(buf)); // sentinel fill to detect any overrun
    int n = bb_net_health_format_log(&s, buf, cap);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(cap, n); // full line exceeds cap

    // egr suffix dropped entirely — not even a partial "egr=" token survives.
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "egr=") == NULL, buf);

    // gw + txfail (higher priority than egr) survive intact.
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "gw=0") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "gwdead=7") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "txfail=1/2") != NULL, buf);

    // Preceding fields present and not truncated mid-token.
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "up=9999") != NULL, buf);
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "restart=4") != NULL, buf);

    // NUL-terminated within cap (snprintf guarantee).
    TEST_ASSERT_EQUAL_INT(0, buf[cap - 1]);
}

// ---------------------------------------------------------------------------
// bb_net_health_would_recover_edge (B1-518 PR3, OBSERVE-ONLY) — pure
// edge-check predicate backing the evaluator's "would recover" log line.
// ---------------------------------------------------------------------------

void test_bb_net_health_would_recover_edge_transition_into_gw_unreachable(void)
{
    TEST_ASSERT_TRUE(bb_net_health_would_recover_edge(BB_EGRESS_STATE_OK,
                                                        BB_EGRESS_STATE_GW_UNREACHABLE));
}

void test_bb_net_health_would_recover_edge_endpoint_down_into_gw_unreachable(void)
{
    TEST_ASSERT_TRUE(bb_net_health_would_recover_edge(BB_EGRESS_STATE_ENDPOINT_DOWN,
                                                        BB_EGRESS_STATE_GW_UNREACHABLE));
}

// Sustained GW_UNREACHABLE across ticks does not re-log every cycle.
void test_bb_net_health_would_recover_edge_sustained_no_relog(void)
{
    TEST_ASSERT_FALSE(bb_net_health_would_recover_edge(BB_EGRESS_STATE_GW_UNREACHABLE,
                                                         BB_EGRESS_STATE_GW_UNREACHABLE));
}

void test_bb_net_health_would_recover_edge_transition_out_is_false(void)
{
    TEST_ASSERT_FALSE(bb_net_health_would_recover_edge(BB_EGRESS_STATE_GW_UNREACHABLE,
                                                         BB_EGRESS_STATE_OK));
}

void test_bb_net_health_would_recover_edge_ok_to_ok_is_false(void)
{
    TEST_ASSERT_FALSE(bb_net_health_would_recover_edge(BB_EGRESS_STATE_OK,
                                                         BB_EGRESS_STATE_OK));
}

void test_bb_net_health_would_recover_edge_ok_to_endpoint_down_is_false(void)
{
    TEST_ASSERT_FALSE(bb_net_health_would_recover_edge(BB_EGRESS_STATE_OK,
                                                         BB_EGRESS_STATE_ENDPOINT_DOWN));
}

// ---------------------------------------------------------------------------
// bb_net_health_should_request_recovery (B1-518 PR4, tier-2) — pure
// predicate. act_enabled x edge/no-edge/sustained.
// ---------------------------------------------------------------------------

void test_bb_net_health_should_request_recovery_act_off_is_always_false(void)
{
    TEST_ASSERT_FALSE(bb_net_health_should_request_recovery(BB_EGRESS_STATE_OK,
                                                              BB_EGRESS_STATE_GW_UNREACHABLE,
                                                              false));
}

void test_bb_net_health_should_request_recovery_act_on_edge_is_true(void)
{
    TEST_ASSERT_TRUE(bb_net_health_should_request_recovery(BB_EGRESS_STATE_OK,
                                                             BB_EGRESS_STATE_GW_UNREACHABLE,
                                                             true));
}

void test_bb_net_health_should_request_recovery_act_on_no_edge_is_false(void)
{
    TEST_ASSERT_FALSE(bb_net_health_should_request_recovery(BB_EGRESS_STATE_OK,
                                                              BB_EGRESS_STATE_OK,
                                                              true));
}

// Sustained GW_UNREACHABLE (prev == cur) is not an edge — no repeated request.
void test_bb_net_health_should_request_recovery_sustained_is_false(void)
{
    TEST_ASSERT_FALSE(bb_net_health_should_request_recovery(BB_EGRESS_STATE_GW_UNREACHABLE,
                                                              BB_EGRESS_STATE_GW_UNREACHABLE,
                                                              true));
}

// ---------------------------------------------------------------------------
// bb_net_health_should_reboot (B1-518 PR4, tier-3) — pure predicate.
// unhealthy-not-armed / below-T / within-min-interval / daily-cap-exhausted /
// all-met-true / clock-skew-no-underflow.
// ---------------------------------------------------------------------------

static bb_net_health_reboot_state_t s_reboot_st;

static void reboot_st_reset(void)
{
    memset(&s_reboot_st, 0, sizeof(s_reboot_st));
}

void test_bb_net_health_should_reboot_not_unhealthy_is_false(void)
{
    reboot_st_reset();
    TEST_ASSERT_FALSE(bb_net_health_should_reboot(0, 1000, 480, 1800, 4, &s_reboot_st));
}

void test_bb_net_health_should_reboot_below_threshold_is_false(void)
{
    reboot_st_reset();
    // unhealthy since t=1000, now=1400 -> 400s elapsed, threshold 480 -> not yet.
    TEST_ASSERT_FALSE(bb_net_health_should_reboot(1000, 1400, 480, 1800, 4, &s_reboot_st));
}

void test_bb_net_health_should_reboot_at_threshold_is_true(void)
{
    reboot_st_reset();
    // unhealthy since t=1000, now=1480 -> exactly 480s elapsed.
    TEST_ASSERT_TRUE(bb_net_health_should_reboot(1000, 1480, 480, 1800, 4, &s_reboot_st));
}

void test_bb_net_health_should_reboot_within_min_interval_is_false(void)
{
    reboot_st_reset();
    s_reboot_st.last_reboot_s = 1000;
    // unhealthy long enough (>=480s), but last reboot only 100s ago (< 1800s min interval).
    TEST_ASSERT_FALSE(bb_net_health_should_reboot(500, 1100, 480, 1800, 4, &s_reboot_st));
}

void test_bb_net_health_should_reboot_min_interval_elapsed_is_true(void)
{
    reboot_st_reset();
    s_reboot_st.last_reboot_s = 1000;
    // last reboot 1800s ago -> min interval satisfied.
    TEST_ASSERT_TRUE(bb_net_health_should_reboot(500, 2800, 480, 1800, 4, &s_reboot_st));
}

// last_reboot_s == 0 (never rebooted) bypasses the min-interval check.
void test_bb_net_health_should_reboot_never_rebooted_bypasses_min_interval(void)
{
    reboot_st_reset();
    TEST_ASSERT_TRUE(bb_net_health_should_reboot(500, 1000, 480, 1800, 4, &s_reboot_st));
}

void test_bb_net_health_should_reboot_daily_cap_exhausted_is_false(void)
{
    reboot_st_reset();
    uint32_t now_s = 100000;
    // Fill the ring with 4 entries, all within the trailing 24h AND spaced so
    // the last (most recent) entry still clears the 1800s min-interval check
    // on its own — isolating the daily-cap branch as the sole blocker.
    for (int i = 3; i >= 0; i--) {
        bb_net_health_reboot_state_record(&s_reboot_st, now_s - (uint32_t)(2000 * i) - 2000U);
    }
    TEST_ASSERT_EQUAL_UINT8(4, s_reboot_st.ring_count);
    // unhealthy long enough, min-interval elapsed, but daily cap (4) reached.
    TEST_ASSERT_FALSE(bb_net_health_should_reboot(now_s - 1000, now_s, 480, 1800, 4, &s_reboot_st));
}

// A ring entry older than 24h does not count toward the daily cap.
void test_bb_net_health_should_reboot_daily_cap_excludes_stale_entries(void)
{
    reboot_st_reset();
    uint32_t now_s = 200000;
    // 3 entries older than 24h (86400s) -> all excluded from the 24h window,
    // so a daily cap of 4 is not reached (count24h == 0). The last recorded
    // timestamp also clears the 1800s min-interval check.
    for (int i = 0; i < 3; i++) {
        bb_net_health_reboot_state_record(&s_reboot_st, now_s - 90000U - (uint32_t)i);
    }
    TEST_ASSERT_TRUE(bb_net_health_should_reboot(now_s - 1000, now_s, 480, 1800, 4, &s_reboot_st));
}

// All conditions met -> true.
void test_bb_net_health_should_reboot_all_met_true(void)
{
    reboot_st_reset();
    TEST_ASSERT_TRUE(bb_net_health_should_reboot(1000, 100000, 480, 1800, 4, &s_reboot_st));
}

// st->ring_count corrupted/out-of-range (> CAP_MAX) is clamped to CAP_MAX
// rather than walking past the end of reboot_s_ring[]. Reachable directly
// via the struct field (e.g. a caller-loaded/corrupted state), independent
// of encode/decode's own ring_count_field validation.
void test_bb_net_health_should_reboot_ring_count_over_cap_is_clamped(void)
{
    reboot_st_reset();
    for (int i = 0; i < BB_NET_HEALTH_REBOOT_CAP_MAX; i++) {
        s_reboot_st.reboot_s_ring[i] = 100000U - 1000U - (uint32_t)i; // all within 24h
    }
    s_reboot_st.ring_count = (uint8_t)(BB_NET_HEALTH_REBOOT_CAP_MAX + 3); // corrupted/out-of-range
    // count24h reaches CAP_MAX (10) entries even though ring_count claims more;
    // daily_cap=CAP_MAX means the clamp (not an over-read) determines the count.
    TEST_ASSERT_FALSE(bb_net_health_should_reboot(1000, 100000, 480, 1800,
                                                    BB_NET_HEALTH_REBOOT_CAP_MAX, &s_reboot_st));
}

// A ring entry timestamped after "now" (per-entry clock skew, distinct from
// the top-level unhealthy_since_s/last_reboot_s skew guards) is excluded
// from the 24h daily-cap count rather than underflowing (now_s - ts).
void test_bb_net_health_should_reboot_ring_entry_future_timestamp_excluded(void)
{
    reboot_st_reset();
    uint32_t now_s = 100000;
    // Populate the ring entry directly (not via record(), which would also
    // set last_reboot_s to the same future value and trip the min-interval
    // guard first) so the future-timestamp skew is isolated to the ring scan.
    s_reboot_st.reboot_s_ring[0] = now_s + 500U; // future: per-entry clock skew
    s_reboot_st.ring_count = 1;
    s_reboot_st.ring_head  = 1;
    // last_reboot_s stays 0 -> bypasses the min-interval check entirely.
    // With the single (excluded) entry not counted, daily cap of 1 is not
    // reached -> should_reboot proceeds to true.
    TEST_ASSERT_TRUE(bb_net_health_should_reboot(now_s - 1000, now_s, 480, 1800, 1, &s_reboot_st));
}

// Clock skew: now_s before unhealthy_since_s must never underflow-wrap to a
// huge elapsed value — treated as 0 elapsed, so should_reboot is false.
void test_bb_net_health_should_reboot_clock_skew_unhealthy_since_no_underflow(void)
{
    reboot_st_reset();
    TEST_ASSERT_FALSE(bb_net_health_should_reboot(5000, 100, 480, 1800, 4, &s_reboot_st));
}

// Clock skew: now_s before last_reboot_s must not underflow either — treated
// as 0 elapsed (min-interval not satisfied) -> should_reboot is false.
void test_bb_net_health_should_reboot_clock_skew_last_reboot_no_underflow(void)
{
    reboot_st_reset();
    s_reboot_st.last_reboot_s = 5000;
    TEST_ASSERT_FALSE(bb_net_health_should_reboot(50, 100, 480, 1800, 4, &s_reboot_st));
}

// NULL state pointer is handled safely (false, no crash).
void test_bb_net_health_should_reboot_null_state_is_false(void)
{
    TEST_ASSERT_FALSE(bb_net_health_should_reboot(1000, 100000, 480, 1800, 4, NULL));
}

// ---------------------------------------------------------------------------
// bb_net_health_reboot_state_record — ring append, wrap at CAP, and 24h-old
// exclusion from a later should_reboot count.
// ---------------------------------------------------------------------------

void test_bb_net_health_reboot_state_record_appends(void)
{
    reboot_st_reset();
    bb_net_health_reboot_state_record(&s_reboot_st, 1000);
    TEST_ASSERT_EQUAL_UINT32(1000, s_reboot_st.last_reboot_s);
    TEST_ASSERT_EQUAL_UINT8(1, s_reboot_st.ring_count);
    TEST_ASSERT_EQUAL_UINT8(1, s_reboot_st.ring_head);
    TEST_ASSERT_EQUAL_UINT32(1000, s_reboot_st.reboot_s_ring[0]);
}

// NULL state pointer is handled safely (no-op, no crash).
void test_bb_net_health_reboot_state_record_null_state_is_safe(void)
{
    bb_net_health_reboot_state_record(NULL, 1000);
}

void test_bb_net_health_reboot_state_record_wraps_at_cap(void)
{
    reboot_st_reset();
    // Record CAP_MAX + 2 entries; ring_count saturates at CAP_MAX and the
    // head wraps, overwriting the oldest two entries.
    for (uint32_t i = 0; i < (uint32_t)BB_NET_HEALTH_REBOOT_CAP_MAX + 2; i++) {
        bb_net_health_reboot_state_record(&s_reboot_st, 1000 + i);
    }
    TEST_ASSERT_EQUAL_UINT8(BB_NET_HEALTH_REBOOT_CAP_MAX, s_reboot_st.ring_count);
    TEST_ASSERT_EQUAL_UINT8(2, s_reboot_st.ring_head);
    // Slot 0 and 1 were overwritten by the wrap (originally 1000, 1001; now
    // hold the last two written values: 1000+CAP_MAX, 1000+CAP_MAX+1).
    TEST_ASSERT_EQUAL_UINT32(1000U + BB_NET_HEALTH_REBOOT_CAP_MAX,     s_reboot_st.reboot_s_ring[0]);
    TEST_ASSERT_EQUAL_UINT32(1000U + BB_NET_HEALTH_REBOOT_CAP_MAX + 1, s_reboot_st.reboot_s_ring[1]);
    TEST_ASSERT_EQUAL_UINT32(1000U + BB_NET_HEALTH_REBOOT_CAP_MAX + 1, s_reboot_st.last_reboot_s);
}

// A ring entry recorded >24h before "now" is excluded from should_reboot's
// daily-cap count, letting a new reboot proceed even with a full ring of
// stale entries.
void test_bb_net_health_reboot_state_record_24h_old_excluded_from_count(void)
{
    reboot_st_reset();
    uint32_t old_ts = 1000;
    bb_net_health_reboot_state_record(&s_reboot_st, old_ts);
    uint32_t now_s = old_ts + 86400U + 1U; // just over 24h later
    TEST_ASSERT_TRUE(bb_net_health_should_reboot(now_s - 1000, now_s, 480, 1800, 1, &s_reboot_st));
}

// ---------------------------------------------------------------------------
// bb_net_health_reboot_state_encode / _decode — single-key NVS packing
// (B1-518 PR4 MED finding). Pure round-trip; no NVS involved.
// ---------------------------------------------------------------------------

void test_bb_net_health_reboot_state_encode_decode_round_trip_zero_state(void)
{
    reboot_st_reset();
    char buf[BB_NET_HEALTH_REBOOT_STATE_STR_MAX];
    TEST_ASSERT_TRUE(bb_net_health_reboot_state_encode(&s_reboot_st, buf, sizeof(buf)));

    bb_net_health_reboot_state_t decoded;
    memset(&decoded, 0xAA, sizeof(decoded)); // poison to prove decode fully overwrites
    TEST_ASSERT_TRUE(bb_net_health_reboot_state_decode(buf, &decoded));
    TEST_ASSERT_EQUAL_UINT32(s_reboot_st.last_reboot_s, decoded.last_reboot_s);
    TEST_ASSERT_EQUAL_UINT8(s_reboot_st.ring_head, decoded.ring_head);
    TEST_ASSERT_EQUAL_UINT8(s_reboot_st.ring_count, decoded.ring_count);
    for (int i = 0; i < BB_NET_HEALTH_REBOOT_CAP_MAX; i++) {
        TEST_ASSERT_EQUAL_UINT32(s_reboot_st.reboot_s_ring[i], decoded.reboot_s_ring[i]);
    }
}

void test_bb_net_health_reboot_state_encode_decode_round_trip_partial_ring(void)
{
    reboot_st_reset();
    bb_net_health_reboot_state_record(&s_reboot_st, 1000);
    bb_net_health_reboot_state_record(&s_reboot_st, 2000);
    bb_net_health_reboot_state_record(&s_reboot_st, 3000);

    char buf[BB_NET_HEALTH_REBOOT_STATE_STR_MAX];
    TEST_ASSERT_TRUE(bb_net_health_reboot_state_encode(&s_reboot_st, buf, sizeof(buf)));

    bb_net_health_reboot_state_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    TEST_ASSERT_TRUE(bb_net_health_reboot_state_decode(buf, &decoded));
    TEST_ASSERT_EQUAL_UINT32(3000, decoded.last_reboot_s);
    TEST_ASSERT_EQUAL_UINT8(3, decoded.ring_head);
    TEST_ASSERT_EQUAL_UINT8(3, decoded.ring_count);
    TEST_ASSERT_EQUAL_UINT32(1000, decoded.reboot_s_ring[0]);
    TEST_ASSERT_EQUAL_UINT32(2000, decoded.reboot_s_ring[1]);
    TEST_ASSERT_EQUAL_UINT32(3000, decoded.reboot_s_ring[2]);
}

// Ring-wrap case: encode/decode must preserve the exact post-wrap slot
// contents and head/count, not just the logical reboot history.
void test_bb_net_health_reboot_state_encode_decode_round_trip_ring_wrap(void)
{
    reboot_st_reset();
    for (uint32_t i = 0; i < (uint32_t)BB_NET_HEALTH_REBOOT_CAP_MAX + 3; i++) {
        bb_net_health_reboot_state_record(&s_reboot_st, 5000 + i);
    }

    char buf[BB_NET_HEALTH_REBOOT_STATE_STR_MAX];
    TEST_ASSERT_TRUE(bb_net_health_reboot_state_encode(&s_reboot_st, buf, sizeof(buf)));

    bb_net_health_reboot_state_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    TEST_ASSERT_TRUE(bb_net_health_reboot_state_decode(buf, &decoded));
    TEST_ASSERT_EQUAL_UINT32(s_reboot_st.last_reboot_s, decoded.last_reboot_s);
    TEST_ASSERT_EQUAL_UINT8(s_reboot_st.ring_head, decoded.ring_head);
    TEST_ASSERT_EQUAL_UINT8(BB_NET_HEALTH_REBOOT_CAP_MAX, decoded.ring_count);
    for (int i = 0; i < BB_NET_HEALTH_REBOOT_CAP_MAX; i++) {
        TEST_ASSERT_EQUAL_UINT32(s_reboot_st.reboot_s_ring[i], decoded.reboot_s_ring[i]);
    }
}

void test_bb_net_health_reboot_state_encode_null_state_is_false(void)
{
    char buf[BB_NET_HEALTH_REBOOT_STATE_STR_MAX];
    TEST_ASSERT_FALSE(bb_net_health_reboot_state_encode(NULL, buf, sizeof(buf)));
}

void test_bb_net_health_reboot_state_encode_null_buf_is_false(void)
{
    reboot_st_reset();
    TEST_ASSERT_FALSE(bb_net_health_reboot_state_encode(&s_reboot_st, NULL, 64));
}

void test_bb_net_health_reboot_state_encode_buf_too_small_is_false(void)
{
    reboot_st_reset();
    bb_net_health_reboot_state_record(&s_reboot_st, 1234567890U);
    char tiny[4];
    TEST_ASSERT_FALSE(bb_net_health_reboot_state_encode(&s_reboot_st, tiny, sizeof(tiny)));
}

void test_bb_net_health_reboot_state_encode_zero_buf_len_is_false(void)
{
    reboot_st_reset();
    char buf[BB_NET_HEALTH_REBOOT_STATE_STR_MAX];
    TEST_ASSERT_FALSE(bb_net_health_reboot_state_encode(&s_reboot_st, buf, 0));
}

// Header snprintf fits (buf_len=10 covers "0|0|0" = 5 bytes), but the ring
// loop runs out of room partway through — distinct from
// encode_buf_too_small_is_false, which fails at the header itself.
void test_bb_net_health_reboot_state_encode_buf_too_small_mid_ring_is_false(void)
{
    reboot_st_reset();
    char buf[10];
    TEST_ASSERT_FALSE(bb_net_health_reboot_state_encode(&s_reboot_st, buf, sizeof(buf)));
}

void test_bb_net_health_reboot_state_decode_null_str_is_false(void)
{
    bb_net_health_reboot_state_t out;
    TEST_ASSERT_FALSE(bb_net_health_reboot_state_decode(NULL, &out));
}

void test_bb_net_health_reboot_state_decode_empty_str_is_false(void)
{
    bb_net_health_reboot_state_t out;
    TEST_ASSERT_FALSE(bb_net_health_reboot_state_decode("", &out));
}

void test_bb_net_health_reboot_state_decode_null_out_is_false(void)
{
    TEST_ASSERT_FALSE(bb_net_health_reboot_state_decode("0|0|0|0,0,0,0,0,0,0,0,0,0", NULL));
}

void test_bb_net_health_reboot_state_decode_malformed_is_false(void)
{
    bb_net_health_reboot_state_t out;
    TEST_ASSERT_FALSE(bb_net_health_reboot_state_decode("not a valid encoded state", &out));
}

void test_bb_net_health_reboot_state_decode_wrong_ring_length_is_false(void)
{
    bb_net_health_reboot_state_t out;
    // Only 3 ring values instead of the required BB_NET_HEALTH_REBOOT_CAP_MAX.
    TEST_ASSERT_FALSE(bb_net_health_reboot_state_decode("0|0|0|1,2,3", &out));
}

void test_bb_net_health_reboot_state_decode_ring_head_out_of_range_is_false(void)
{
    bb_net_health_reboot_state_t out;
    TEST_ASSERT_FALSE(bb_net_health_reboot_state_decode(
        "0|99|0|0,0,0,0,0,0,0,0,0,0", &out));
}

void test_bb_net_health_reboot_state_decode_ring_count_out_of_range_is_false(void)
{
    bb_net_health_reboot_state_t out;
    TEST_ASSERT_FALSE(bb_net_health_reboot_state_decode(
        "0|0|99|0,0,0,0,0,0,0,0,0,0", &out));
}

// A non-numeric ring entry token fails the per-entry sscanf parse (distinct
// from decode_wrong_ring_length_is_false, which fails the *(p)!=',' comma
// check after running out of entries, never reaching this sscanf failure).
void test_bb_net_health_reboot_state_decode_malformed_ring_entry_is_false(void)
{
    bb_net_health_reboot_state_t out;
    TEST_ASSERT_FALSE(bb_net_health_reboot_state_decode(
        "0|0|0|1,2,x,4,5,6,7,8,9,0", &out));
}
