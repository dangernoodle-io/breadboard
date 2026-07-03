// Host tests for bb_net_health pure classifier.
// Covers:
//   - every RSSI bucket boundary (GOOD / MARGINAL / POOR)
//   - hysteresis downgrade (N consecutive worse samples required)
//   - hysteresis upgrade (N consecutive better samples required)
//   - early_warning on each trigger: sustained-poor, reconnect-increase, disconnect
//   - warn_disc uses mqtt_disc_age_s (not wifi disc_age_s)
//   - bb_net_state_str helper
//   - bb_net_health_throttle_decision: throttle start + restore
//   - bb_net_health_emit: nested mqtt sub-object; OOM branch coverage
#include "unity.h"
#include "bb_net_health.h"
#include "bb_json.h"
#include "bb_json_test_hooks.h"
#include "bb_event.h"
#include "bb_event_ring.h"

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
// RSSI bucket boundary tests (with fresh state — single eval)
// ---------------------------------------------------------------------------

// BUG 1 regression: rssi==0 (no valid reading) must classify as POOR.
void test_bb_net_health_rssi_zero_is_poor(void)
{
    reset();
    s_in.rssi = 0;
    eval();  // cold-start: seeds directly from raw bucket
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_POOR, s_out.state);
    TEST_ASSERT_TRUE(s_out.early_warning);  // POOR → early_warning
}

// BUG 1 regression: any positive rssi must classify as POOR.
void test_bb_net_health_rssi_positive_is_poor(void)
{
    reset();
    s_in.rssi = 10;
    eval();
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_POOR, s_out.state);
}

// BUG 2 regression: cold-start with poor rssi must report POOR immediately
// (not GOOD via hysteresis lag).
void test_bb_net_health_cold_start_poor_rssi_reports_poor(void)
{
    reset();
    s_in.rssi = -80;  // raw bucket = POOR
    eval();           // first eval — seeds, bypasses hysteresis
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_POOR, s_out.state);
    TEST_ASSERT_TRUE(s_out.early_warning);
}

// BUG 2 regression: cold-start with good rssi must report GOOD immediately.
void test_bb_net_health_cold_start_good_rssi_reports_good(void)
{
    reset();
    s_in.rssi = -50;  // raw bucket = GOOD
    eval();           // first eval — seeds
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_GOOD, s_out.state);
}

// Hysteresis applies starting from the 2nd eval — a single POOR sample after
// a seeded-GOOD first eval must NOT immediately flip to POOR.
void test_bb_net_health_hyst_applies_after_cold_start(void)
{
    reset();
    s_in.rssi = -50;  // seed GOOD on first eval
    eval();
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_GOOD, s_out.state);

    // Second eval with POOR rssi — hysteresis: still GOOD after 1 sample.
    s_in.rssi = -80;
    eval();
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_GOOD, s_out.state);

    // Need HYST_DOWN consecutive POOR to downgrade.
    for (int i = 1; i < BB_NET_HEALTH_HYST_DOWN; i++) {
        eval();
    }
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_POOR, s_out.state);
}

void test_bb_net_health_rssi_good_boundary(void)
{
    reset();
    s_in.rssi = -67;  // exactly at GOOD threshold
    eval();
    // First eval seeds directly — GOOD.
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_GOOD, s_out.state);
}

void test_bb_net_health_rssi_marginal_lo(void)
{
    reset();
    // Start in GOOD, then feed MARGINAL N times to trigger downgrade.
    s_in.rssi = -68;  // first MARGINAL sample (just below GOOD threshold)
    eval_n(BB_NET_HEALTH_HYST_DOWN);
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_MARGINAL, s_out.state);
}

void test_bb_net_health_rssi_marginal_hi(void)
{
    reset();
    s_in.rssi = -75;  // exactly at MARGINAL_LO threshold (still MARGINAL)
    eval_n(BB_NET_HEALTH_HYST_DOWN);
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_MARGINAL, s_out.state);
}

void test_bb_net_health_rssi_poor_below_marginal_lo(void)
{
    reset();
    s_in.rssi = -76;  // one below MARGINAL_LO → POOR
    eval_n(BB_NET_HEALTH_HYST_DOWN);
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_POOR, s_out.state);
}

void test_bb_net_health_rssi_very_poor(void)
{
    reset();
    s_in.rssi = -95;
    eval_n(BB_NET_HEALTH_HYST_DOWN);
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_POOR, s_out.state);
}

// ---------------------------------------------------------------------------
// Hysteresis downgrade tests
// ---------------------------------------------------------------------------

void test_bb_net_health_hyst_down_requires_n_samples(void)
{
    reset();
    s_in.rssi = -50;
    eval();  // establish GOOD
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_GOOD, s_out.state);

    // Feed MARGINAL signal — should NOT downgrade until HYST_DOWN consecutive.
    s_in.rssi = -70;
    for (int i = 0; i < BB_NET_HEALTH_HYST_DOWN - 1; i++) {
        eval();
        TEST_ASSERT_EQUAL_INT(BB_NET_STATE_GOOD, s_out.state);
    }
    // One more — now should downgrade.
    eval();
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_MARGINAL, s_out.state);
}

void test_bb_net_health_hyst_down_reset_on_good_sample(void)
{
    reset();
    s_in.rssi = -50;
    eval();  // establish GOOD

    // Feed HYST_DOWN-1 MARGINAL samples, then one GOOD — counter resets.
    s_in.rssi = -70;
    for (int i = 0; i < BB_NET_HEALTH_HYST_DOWN - 1; i++) {
        eval();
    }
    s_in.rssi = -50;  // back to GOOD — resets down_count
    eval();
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_GOOD, s_out.state);

    // Another HYST_DOWN MARGINAL samples should be needed for downgrade.
    s_in.rssi = -70;
    for (int i = 0; i < BB_NET_HEALTH_HYST_DOWN - 1; i++) {
        eval();
        TEST_ASSERT_EQUAL_INT(BB_NET_STATE_GOOD, s_out.state);
    }
    eval();
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_MARGINAL, s_out.state);
}

// ---------------------------------------------------------------------------
// Hysteresis upgrade tests
// ---------------------------------------------------------------------------

void test_bb_net_health_hyst_up_requires_n_samples(void)
{
    reset();
    // Drive to POOR first.
    s_in.rssi = -80;
    eval_n(BB_NET_HEALTH_HYST_DOWN);
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_POOR, s_out.state);

    // Feed MARGINAL — should NOT upgrade until HYST_UP consecutive.
    s_in.rssi = -70;
    for (int i = 0; i < BB_NET_HEALTH_HYST_UP - 1; i++) {
        eval();
        TEST_ASSERT_EQUAL_INT(BB_NET_STATE_POOR, s_out.state);
    }
    eval();
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_MARGINAL, s_out.state);
}

void test_bb_net_health_hyst_up_reset_on_poor_sample(void)
{
    reset();
    // Drive to POOR.
    s_in.rssi = -80;
    eval_n(BB_NET_HEALTH_HYST_DOWN);
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_POOR, s_out.state);

    // Feed HYST_UP-1 MARGINAL samples, then one POOR — up_count resets.
    s_in.rssi = -70;
    for (int i = 0; i < BB_NET_HEALTH_HYST_UP - 1; i++) {
        eval();
    }
    s_in.rssi = -80;  // back to POOR — resets up_count
    eval();
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_POOR, s_out.state);

    // Now feed HYST_UP MARGINAL samples — should upgrade.
    s_in.rssi = -70;
    eval_n(BB_NET_HEALTH_HYST_UP);
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_MARGINAL, s_out.state);
}

void test_bb_net_health_hyst_up_all_the_way_to_good(void)
{
    reset();
    // Drive to POOR.
    s_in.rssi = -80;
    eval_n(BB_NET_HEALTH_HYST_DOWN);
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_POOR, s_out.state);

    // Upgrade to MARGINAL.
    s_in.rssi = -70;
    eval_n(BB_NET_HEALTH_HYST_UP);
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_MARGINAL, s_out.state);

    // Upgrade to GOOD.
    s_in.rssi = -50;
    eval_n(BB_NET_HEALTH_HYST_UP);
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_GOOD, s_out.state);
}

// ---------------------------------------------------------------------------
// early_warning trigger: sustained POOR
// ---------------------------------------------------------------------------

void test_bb_net_health_early_warning_sustained_poor(void)
{
    reset();
    // Drive to POOR — early_warning should be true once in POOR state.
    s_in.rssi = -80;
    eval_n(BB_NET_HEALTH_HYST_DOWN);
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_POOR, s_out.state);
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
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_GOOD, s_out.state);
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
// bb_net_state_str helper
// ---------------------------------------------------------------------------

void test_bb_net_state_str_good(void)
{
    TEST_ASSERT_EQUAL_STRING("good", bb_net_state_str(BB_NET_STATE_GOOD));
}

void test_bb_net_state_str_marginal(void)
{
    TEST_ASSERT_EQUAL_STRING("marginal", bb_net_state_str(BB_NET_STATE_MARGINAL));
}

void test_bb_net_state_str_poor(void)
{
    TEST_ASSERT_EQUAL_STRING("poor", bb_net_state_str(BB_NET_STATE_POOR));
}

void test_bb_net_state_str_unknown_returns_nonnull(void)
{
    // Unknown enum values should not return NULL.
    const char *s = bb_net_state_str((bb_net_state_t)99);
    TEST_ASSERT_NOT_NULL(s);
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
    eval_n(BB_NET_HEALTH_HYST_DOWN);
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_POOR, s_out.state);

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
    eval_n(BB_NET_HEALTH_HYST_DOWN + 3);  // ensure sustained count >= 3
    bb_net_health_throttle_decision(&s_st, 3);  // should set throttled
    // If for some reason still not throttled, force it.
    s_st.throttled            = true;
    s_st.sustained_poor_count = 5;

    // Recover to GOOD.
    s_in.rssi = -50;
    eval_n(BB_NET_HEALTH_HYST_UP);
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_GOOD, s_out.state);

    bool t = bb_net_health_throttle_decision(&s_st, 3);
    TEST_ASSERT_FALSE(t);
    TEST_ASSERT_FALSE(s_st.throttled);
}

void test_bb_net_health_throttle_restores_on_marginal(void)
{
    reset();
    // Force throttled state with POOR.
    s_in.rssi = -80;
    eval_n(BB_NET_HEALTH_HYST_DOWN);
    s_st.throttled            = true;
    s_st.sustained_poor_count = 5;

    // Recover to MARGINAL.
    s_in.rssi = -70;
    eval_n(BB_NET_HEALTH_HYST_UP);
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_MARGINAL, s_out.state);

    bool t = bb_net_health_throttle_decision(&s_st, 3);
    TEST_ASSERT_FALSE(t);
}

void test_bb_net_health_throttle_no_restore_while_poor(void)
{
    reset();
    s_in.rssi = -80;
    eval_n(BB_NET_HEALTH_HYST_DOWN + 3);
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
    eval_n(BB_NET_HEALTH_HYST_DOWN + 3);

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
    eval_n(BB_NET_HEALTH_HYST_UP);

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
    eval_n(BB_NET_HEALTH_HYST_DOWN);
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_POOR, s_out.state);

    s_in.mqtt_reconnect_count = 3;
    eval();
    TEST_ASSERT_TRUE(s_out.early_warning);
    TEST_ASSERT_EQUAL_INT(BB_NET_STATE_POOR, s_out.state);
}

// ---------------------------------------------------------------------------
// SSE payload size contract
// ---------------------------------------------------------------------------

// The net.health SSE topic now emits the full payload (nested mqtt) via bb_cache.
// Worst-case input: rssi=-128, state="marginal" (longest), all bools true,
// large integer fields.  Assert the serialised length fits in a 512-byte ring
// slot (nested mqtt adds ~80 bytes over the old flat layout).
void test_bb_net_health_sse_payload_fits_256_byte_ring_slot(void)
{
    bb_net_health_status_t snap = {
        .state                  = BB_NET_STATE_MARGINAL,
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
        .state                  = BB_NET_STATE_GOOD,
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

    double dr = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "last_disconnect_reason", &dr));
    TEST_ASSERT_EQUAL_INT(3, (int)dr);

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
        .state                  = BB_NET_STATE_POOR,
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

    double dr = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(parsed, "last_disconnect_reason", &dr));
    TEST_ASSERT_EQUAL_INT(5, (int)dr);

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
        .state         = BB_NET_STATE_MARGINAL,
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
        .state                  = BB_NET_STATE_GOOD,
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
        .state                  = BB_NET_STATE_MARGINAL,
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
        .state                  = BB_NET_STATE_GOOD,
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

// ---------------------------------------------------------------------------
// bb_net_health_emit: nested http sub-object
// ---------------------------------------------------------------------------

void test_bb_net_health_emit_http_object_present(void)
{
    bb_net_health_status_t snap = {
        .state                  = BB_NET_STATE_GOOD,
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
        .http_connected         = true,
        .http_consec_failures   = 2,
        .http_tls_fail          = 1,
        .http_last_status       = 503,
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

    bb_json_t http_obj = bb_json_obj_get_item(parsed, "http");
    TEST_ASSERT_NOT_NULL(http_obj);

    bool conn = false;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(http_obj, "connected", &conn));
    TEST_ASSERT_TRUE(conn);

    double cf = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(http_obj, "consec_failures", &cf));
    TEST_ASSERT_EQUAL_INT(2, (int)cf);

    double tf = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(http_obj, "tls_fail", &tf));
    TEST_ASSERT_EQUAL_INT(1, (int)tf);

    double ls = 0.0;
    TEST_ASSERT_TRUE(bb_json_obj_get_number(http_obj, "last_status", &ls));
    TEST_ASSERT_EQUAL_INT(503, (int)ls);

    // mqtt sub-object must still be present.
    TEST_ASSERT_NOT_NULL(bb_json_obj_get_item(parsed, "mqtt"));

    bb_json_free(parsed);
}

void test_bb_net_health_emit_http_alloc_fail(void)
{
    bb_net_health_status_t snap = {
        .state                  = BB_NET_STATE_GOOD,
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
        .http_connected         = false,
        .http_consec_failures   = 0,
        .http_tls_fail          = 0,
        .http_last_status       = 0,
    };

    // Call 0: outer bb_json_obj_new() — succeeds.
    // Call 1: mqtt sub-object — succeeds.
    // Call 2: http sub-object — fails.
    // fail_after(2): 2 allocs succeed, then the next fails.
    bb_json_host_force_alloc_fail_after(2);
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

    // mqtt must be present (its alloc succeeded).
    TEST_ASSERT_NOT_NULL(bb_json_obj_get_item(parsed, "mqtt"));

    // http key must be absent (alloc failed).
    TEST_ASSERT_NULL(bb_json_obj_get_item(parsed, "http"));

    bb_json_free(parsed);
}

// --- lost-IP telemetry fields ---

void test_bb_net_health_emit_lost_ip_fields(void)
{
    bb_net_health_status_t snap = {
        .state                  = BB_NET_STATE_GOOD,
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
        .http_connected         = false,
        .http_consec_failures   = 0,
        .http_tls_fail          = 0,
        .http_last_status       = 0,
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
        .state                  = BB_NET_STATE_GOOD,
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

// emit_status emits state/early_warning/throttled + mqtt.connected + http.connected
// and must NOT emit any numeric fields (rssi, disc_age_s, reconnect_count, etc.).
void test_bb_net_health_emit_status_status_only(void)
{
    bb_net_health_status_t snap = {
        .state                  = BB_NET_STATE_MARGINAL,
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
        .http_connected         = false,
        .http_consec_failures   = 2,
        .http_tls_fail          = 0,
        .http_last_status       = 503,
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

    // http sub-object: only connected (bool), no counters.
    bb_json_t http_obj = bb_json_obj_get_item(parsed, "http");
    TEST_ASSERT_NOT_NULL_MESSAGE(http_obj, "http sub-object missing from emit_status");
    bool hc = true;
    TEST_ASSERT_TRUE(bb_json_obj_get_bool(http_obj, "connected", &hc));
    TEST_ASSERT_FALSE(hc);

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
        .state          = BB_NET_STATE_GOOD,
        .early_warning  = false,
        .throttled      = false,
        .mqtt_connected = true,
        .http_connected = false,
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

// OOM branch: http sub-object alloc fails — mqtt is present, "http" absent.
void test_bb_net_health_emit_status_http_alloc_fail(void)
{
    bb_net_health_status_t snap = {
        .state          = BB_NET_STATE_GOOD,
        .early_warning  = false,
        .throttled      = false,
        .mqtt_connected = true,
        .http_connected = true,
    };
    // Call 0: outer obj — succeeds.
    // Call 1: mqtt sub-object — succeeds.
    // Call 2: http sub-object — fails.
    bb_json_host_force_alloc_fail_after(2);
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

    // mqtt must be present (its alloc succeeded).
    TEST_ASSERT_NOT_NULL(bb_json_obj_get_item(parsed, "mqtt"));
    // http must be absent (alloc failed).
    TEST_ASSERT_NULL(bb_json_obj_get_item(parsed, "http"));

    bb_json_free(parsed);
}

// ---------------------------------------------------------------------------
// B1-472: retained net.health ring must actually capture the full snapshot.
//
// On HW the serialized net.health snapshot (nested mqtt/http objects) came in
// at ~341 B — above the bb_event_routes global default ring max_entry (256),
// so the retained push was rejected (bb_ring: "push rejected: len=341 >
// max_entry=256") and SSE clients connecting to ?topic=net.health saw empty
// state until the next periodic re-publish. The fix attaches net.health with
// an explicit max_entry (BB_NET_HEALTH_SSE_MAX_ENTRY, 512, defined in
// bb_net_health.h and used directly at the bb_event_routes_attach_ex2 call
// site in bb_net_health_espidf.c), mirroring the update.available /
// info.build precedent (#616, B1-434/435/439). This test measures the
// synthetic snapshot below at ~352 B — a few bytes above the 341 B HW figure
// purely from wider digit-width field values (e.g. disc_age_s=130 vs a
// smaller HW value), not a real discrepancy. Referencing
// BB_NET_HEALTH_SSE_MAX_ENTRY here (rather than a bare 512 literal) means a
// future change to the production value is forced to touch this test too.
// This test exercises the same bb_event_ring seam the ESP-IDF glue uses,
// with a realistic full snapshot (nested mqtt + http, non-zero counters),
// and asserts:
//   1. the serialized payload size (measure, printed via TEST_MESSAGE)
//   2. a ring sized at the old global default (256) DROPS the push (regression)
//   3. a ring sized at BB_NET_HEALTH_SSE_MAX_ENTRY (the fix) CAPTURES it (count == 1)
// bb_net_health_attach_sse() itself is ESP-IDF-only (not reachable from this
// host harness), so this shared constant is the accepted mitigation per
// review — a regression to a different literal at the call site, or a
// revert to the 2-arg bb_event_routes_attach_ex, would desync the value used
// here from production and fail this test the next time the snapshot grows.
// ---------------------------------------------------------------------------

static void bb_net_health_test_setup_sync_mode(void)
{
    setenv("BB_EVENT_HOST_SYNC", "1", 1);
}

void test_bb_net_health_retained_ring_captures_full_snapshot(void)
{
    bb_net_health_test_setup_sync_mode();
    bb_event_cfg_t cfg = { .queue_depth = 8, .max_payload = 512 };
    bb_event_init(&cfg);

    // Realistic full snapshot: nested mqtt + http objects, non-zero counters
    // (mirrors what bb_net_health_espidf.c's publish_snapshot builds on device).
    bb_net_health_status_t snap = {
        .state                  = BB_NET_STATE_MARGINAL,
        .early_warning          = true,
        .throttled              = false,
        .rssi                   = -72,
        .mqtt_connected         = true,
        .mqtt_reconnect_count   = 4,
        .last_disconnect_reason = 8,
        .disc_age_s             = 130,
        .mqtt_disc_age_s        = 45,
        .mqtt_disc_reason       = 2,
        .mqtt_tls_fail          = 1,
        .http_connected         = true,
        .http_consec_failures   = 3,
        .http_tls_fail          = 1,
        .http_last_status       = 503,
        .lost_ip_recoveries     = 2,
        .lost_ip_age_s          = 600,
        .egress_dead_recoveries = 1,
    };

    bb_json_t obj = bb_json_obj_new();
    TEST_ASSERT_NOT_NULL(obj);
    bb_net_health_emit(obj, &snap);
    char *json = bb_json_serialize(obj);
    bb_json_free(obj);
    TEST_ASSERT_NOT_NULL(json);

    size_t len = strlen(json);
    char msg[64];
    snprintf(msg, sizeof(msg), "net.health snapshot serialized len=%zu", len);
    TEST_MESSAGE(msg);

    // Must exceed the old global default (256) — this is what caused the
    // production drop — and fit comfortably within BB_NET_HEALTH_SSE_MAX_ENTRY,
    // the same constant bb_net_health_attach_sse() passes to
    // bb_event_routes_attach_ex2 on device. Bounding against the shared
    // symbol (not a bare 512 literal) means the headroom tracked here moves
    // in lockstep with the production value.
    TEST_ASSERT_GREATER_THAN_size_t(256, len);
    TEST_ASSERT_LESS_THAN_size_t(BB_NET_HEALTH_SSE_MAX_ENTRY, len);

    // Regression: a ring sized at the old global default (256) drops the push.
    bb_event_topic_t topic_old = NULL;
    bb_event_topic_register("net.health.test.old", &topic_old);
    bb_event_ring_t ring_old = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_ring_attach_ex(topic_old, 1, 256, true, &ring_old));
    bb_event_post(topic_old, 1, json, len);
    bb_event_pump(0);
    TEST_ASSERT_EQUAL_size_t(0, bb_event_ring_count(ring_old));
    bb_event_ring_detach(ring_old);

    // Fix: a ring sized at BB_NET_HEALTH_SSE_MAX_ENTRY (the production value)
    // captures it.
    bb_event_topic_t topic_new = NULL;
    bb_event_topic_register("net.health.test.new", &topic_new);
    bb_event_ring_t ring_new = NULL;
    TEST_ASSERT_EQUAL(BB_OK, bb_event_ring_attach_ex(
        topic_new, 1, BB_NET_HEALTH_SSE_MAX_ENTRY, true, &ring_new));
    bb_event_post(topic_new, 1, json, len);
    bb_event_pump(0);
    TEST_ASSERT_EQUAL_size_t(1, bb_event_ring_count(ring_new));
    bb_event_ring_detach(ring_new);

    bb_json_free_str(json);
}

// ---------------------------------------------------------------------------
// bb_net_health_classify_mode — pure WiFi discrimination classifier
// (wifi-netmode PR, observe-only).
// ---------------------------------------------------------------------------

void test_bb_net_health_classify_mode_ok(void)
{
    TEST_ASSERT_EQUAL_INT(BB_NET_MODE_OK, bb_net_health_classify_mode(true, true));
}

void test_bb_net_health_classify_mode_no_ip(void)
{
    TEST_ASSERT_EQUAL_INT(BB_NET_MODE_NO_IP, bb_net_health_classify_mode(true, false));
}

void test_bb_net_health_classify_mode_not_associated(void)
{
    TEST_ASSERT_EQUAL_INT(BB_NET_MODE_NOT_ASSOCIATED, bb_net_health_classify_mode(false, false));
}

// Not-associated dominates has_ip=true (an inconsistent/impossible input on
// real hardware, but the classifier must still resolve deterministically).
void test_bb_net_health_classify_mode_not_associated_dominates_has_ip(void)
{
    TEST_ASSERT_EQUAL_INT(BB_NET_MODE_NOT_ASSOCIATED, bb_net_health_classify_mode(false, true));
}

// ---------------------------------------------------------------------------
// bb_net_health_classify_egress — pure egress-state classifier
// (egress-recovery SSOT, B1-518, Phase 1, OBSERVE-ONLY).
// ---------------------------------------------------------------------------

// wifi_mode != OK short-circuits to OK regardless of any other input.
void test_bb_net_health_classify_egress_no_ip_mode_is_ok(void)
{
    TEST_ASSERT_EQUAL_INT(BB_EGRESS_STATE_OK,
        bb_net_health_classify_egress(BB_NET_MODE_NO_IP, true, false, 5, 3, 2, 2));
}

void test_bb_net_health_classify_egress_not_associated_mode_is_ok(void)
{
    TEST_ASSERT_EQUAL_INT(BB_EGRESS_STATE_OK,
        bb_net_health_classify_egress(BB_NET_MODE_NOT_ASSOCIATED, true, false, 5, 3, 2, 2));
}

// No probe data yet → OK.
void test_bb_net_health_classify_egress_not_probed_is_ok(void)
{
    TEST_ASSERT_EQUAL_INT(BB_EGRESS_STATE_OK,
        bb_net_health_classify_egress(BB_NET_MODE_OK, false, false, 0, 3, 2, 2));
}

// !gw_reachable, streak below threshold → transient miss, still OK.
void test_bb_net_health_classify_egress_gw_unreachable_below_threshold_is_ok(void)
{
    TEST_ASSERT_EQUAL_INT(BB_EGRESS_STATE_OK,
        bb_net_health_classify_egress(BB_NET_MODE_OK, true, false, 2, 3, 0, 0));
}

// !gw_reachable, streak == threshold → GW_UNREACHABLE.
void test_bb_net_health_classify_egress_gw_unreachable_at_threshold(void)
{
    TEST_ASSERT_EQUAL_INT(BB_EGRESS_STATE_GW_UNREACHABLE,
        bb_net_health_classify_egress(BB_NET_MODE_OK, true, false, 3, 3, 0, 0));
}

// gw_reachable, failing == 0 → OK.
void test_bb_net_health_classify_egress_gw_reachable_none_failing(void)
{
    TEST_ASSERT_EQUAL_INT(BB_EGRESS_STATE_OK,
        bb_net_health_classify_egress(BB_NET_MODE_OK, true, true, 0, 3, 2, 0));
}

// gw_reachable, 0 < failing < enabled → ENDPOINT_DOWN. This is the whole
// point of the gateway-probe-as-tiebreaker: gw up + one endpoint down (e.g.
// mining pool) must NOT classify as a WiFi fault.
void test_bb_net_health_classify_egress_gw_reachable_one_endpoint_down(void)
{
    TEST_ASSERT_EQUAL_INT(BB_EGRESS_STATE_ENDPOINT_DOWN,
        bb_net_health_classify_egress(BB_NET_MODE_OK, true, true, 0, 3, 2, 1));
}

// gw_reachable, failing == enabled (> 0) → ALL_DEAD.
void test_bb_net_health_classify_egress_gw_reachable_all_endpoints_down(void)
{
    TEST_ASSERT_EQUAL_INT(BB_EGRESS_STATE_ALL_DEAD,
        bb_net_health_classify_egress(BB_NET_MODE_OK, true, true, 0, 3, 2, 2));
}

// gw_reachable, enabled == 0 → OK (no egress clients configured at all).
void test_bb_net_health_classify_egress_gw_reachable_no_egress_clients(void)
{
    TEST_ASSERT_EQUAL_INT(BB_EGRESS_STATE_OK,
        bb_net_health_classify_egress(BB_NET_MODE_OK, true, true, 0, 3, 0, 0));
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

void test_bb_net_mode_str_ok(void)
{
    TEST_ASSERT_EQUAL_STRING("ok", bb_net_mode_str(BB_NET_MODE_OK));
}

void test_bb_net_mode_str_no_ip(void)
{
    TEST_ASSERT_EQUAL_STRING("no_ip", bb_net_mode_str(BB_NET_MODE_NO_IP));
}

void test_bb_net_mode_str_not_associated(void)
{
    TEST_ASSERT_EQUAL_STRING("not_associated", bb_net_mode_str(BB_NET_MODE_NOT_ASSOCIATED));
}

void test_bb_net_mode_str_unknown_returns_not_associated(void)
{
    const char *s = bb_net_mode_str((bb_net_mode_t)99);
    TEST_ASSERT_EQUAL_STRING("not_associated", s);
}

// ---------------------------------------------------------------------------
// bb_net_health_emit: net_mode/associated/has_ip + the no_ip_recoveries/roam
// serialization-gap fix (they were captured in the status struct but never
// emitted to the net.health payload).
// ---------------------------------------------------------------------------

void test_bb_net_health_emit_has_net_mode_and_discriminator_fields(void)
{
    bb_net_health_status_t snap = {
        .state           = BB_NET_STATE_GOOD,
        .rssi            = -55,
        .no_ip_recoveries = 3,
        .roam_count       = 2,
        .roam_age_s       = 45,
        .net_mode         = BB_NET_MODE_NO_IP,
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
        .state           = BB_NET_STATE_GOOD,
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
        .state          = BB_NET_STATE_GOOD,
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
        .state          = BB_NET_STATE_GOOD,
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
                                       BB_NET_MODE_NO_IP, BB_NET_MODE_OK,
                                       /*interval_s=*/60);
    TEST_ASSERT_TRUE(r);
}

// mode unchanged, interval elapsed -> log.
void test_bb_net_health_should_log_interval_elapsed(void)
{
    int64_t interval_us = 60LL * 1000000LL;
    bool r = bb_net_health_should_log(/*now_us=*/interval_us, /*last_log_us=*/0,
                                       BB_NET_MODE_OK, BB_NET_MODE_OK,
                                       /*interval_s=*/60);
    TEST_ASSERT_TRUE(r);
}

// mode unchanged, interval not yet elapsed -> do not log.
void test_bb_net_health_should_log_neither(void)
{
    bool r = bb_net_health_should_log(/*now_us=*/1000000, /*last_log_us=*/0,
                                       BB_NET_MODE_OK, BB_NET_MODE_OK,
                                       /*interval_s=*/60);
    TEST_ASSERT_FALSE(r);
}

// mode changed AND interval elapsed -> still log (edge wins, no double logic).
void test_bb_net_health_should_log_both(void)
{
    int64_t interval_us = 60LL * 1000000LL;
    bool r = bb_net_health_should_log(/*now_us=*/interval_us + 5, /*last_log_us=*/0,
                                       BB_NET_MODE_NOT_ASSOCIATED, BB_NET_MODE_OK,
                                       /*interval_s=*/60);
    TEST_ASSERT_TRUE(r);
}

// Non-monotonic clock guard: now_us < last_log_us on an unchanged mode must
// not log (elapsed clamped to 0, never negative-elapsed >= interval).
void test_bb_net_health_should_log_clock_went_backwards(void)
{
    bool r = bb_net_health_should_log(/*now_us=*/0, /*last_log_us=*/5000,
                                       BB_NET_MODE_OK, BB_NET_MODE_OK,
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
    s.net_mode                = BB_NET_MODE_NO_IP;
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
    s.net_mode = BB_NET_MODE_OK;
    char buf[256];
    bb_net_health_format_log(&s, buf, sizeof(buf));
    TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "nm=ok") != NULL, buf);
}

void test_bb_net_health_format_log_net_mode_not_associated(void)
{
    bb_net_health_status_t s = sample_status_for_log();
    s.net_mode = BB_NET_MODE_NOT_ASSOCIATED;
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
