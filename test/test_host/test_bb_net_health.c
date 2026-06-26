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

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

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
