// Host unit tests for the 6 producer wire-primitive descriptors added in
// B1-1045 PR-2 (cutover composition-root ownership decision KB 1454). Each
// section (except 4a, see below) proves bb_serialize_json_render(desc,
// &fixture, ...) byte-exact against the SAME producer's CURRENT
// (pre-cutover) serializer output for an identical fixture -- the
// deliverable that proves each producer's later swap to bb_data will be
// byte-preserving on the wire. Mirrors test_snap_desc.c's render_eq()
// helper shape.
//
// Section 4a (bb_diag/diag.boot) has ALREADY been cut over (B1-1053 PR1):
// its bb_json bb_cache serializer is deleted, so its tests pin against
// hand-written literal goldens instead of a live "old" serializer -- see
// that section's own header comment.
//
// No bb_data_bind()/bb_data_http_attach()/bb_data_touch() call exists in
// this file for the still-pre-cutover producers (4b-4f) -- those remain
// descriptor+gather PRIMITIVES only, each producer's own migration PR owns
// wiring it to bb_data (diag.boot's own bb_diag_boot_bind() is the
// exception, added by its cutover -- see bb_diag_boot_wire.c).

#include "unity.h"

#include "bb_serialize_json.h"

#include "bb_cache.h"
#include "bb_log_event_wire.h"
#include "bb_ota_hooks.h"
#include "bb_ota_hooks_wire.h"
#include "bb_health_stack.h"
#include "bb_health_stack_wire.h"
#include "bb_diag_event_priv.h"
#include "bb_diag_boot_wire.h"
#include "bb_display_info_event_priv.h"
#include "bb_display_info_wire.h"
#include "bb_ota_check_internal.h"
#include "bb_ota_check_wire.h"
#include "bb_json.h"

#include <string.h>

// Test reset hook — declared in bb_cache_espidf.c (BB_CACHE_TESTING), mirrors
// test_bb_cache_fidelity.c's local declaration.
void bb_cache_reset_for_test(void);

static void render_eq(const bb_serialize_desc_t *d, const void *snap, const char *golden)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK, bb_serialize_json_render(d, snap, buf, sizeof buf, &n));
    TEST_ASSERT_EQUAL_STRING(golden, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(golden), n);
}

// ---------------------------------------------------------------------------
// 1. bb_log_event -- key "log", single-field passthrough
// ---------------------------------------------------------------------------

void test_wire_desc_log_render_plain(void)
{
    bb_log_event_wire_t snap;
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.log, "{\"ts\":1,\"level\":\"I\",\"tag\":\"app\",\"msg\":\"hello\"}", sizeof(snap.log) - 1);

    render_eq(&bb_log_event_wire_desc, &snap,
              "{\"log\":\"{\\\"ts\\\":1,\\\"level\\\":\\\"I\\\",\\\"tag\\\":\\\"app\\\",\\\"msg\\\":\\\"hello\\\"}\"}");
}

// Embedded-quote/backslash case -- proves the wire string is a JSON-string
// (escaped), never re-parsed as JSON itself.
void test_wire_desc_log_render_escapes_quotes_and_backslashes(void)
{
    bb_log_event_wire_t snap;
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.log, "He said \"hi\" and used a\\backslash", sizeof(snap.log) - 1);

    render_eq(&bb_log_event_wire_desc, &snap,
              "{\"log\":\"He said \\\"hi\\\" and used a\\\\backslash\"}");
}

// ---------------------------------------------------------------------------
// 2. bb_ota_hooks -- key "ota.progress" -- all 4 phases + unknown fallback,
// byte-equal to bb_ota_progress_json().
// ---------------------------------------------------------------------------

// Drives the REAL bb_ota_hooks_gather() via bb_ota_emit_progress() (the
// stash-writer), proving the gather round-trip -- not just a hand-built
// fixture -- byte-equal to bb_ota_progress_json().
static void ota_hooks_render_eq_json_builder(const char *via, bb_ota_phase_t phase, int state, int pct)
{
#ifdef BB_OTA_HOOKS_TESTING
    bb_ota_hooks_test_reset();
#endif
    bb_ota_emit_progress(via, phase, pct);

    bb_ota_hooks_wire_t wire;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_ota_hooks_gather(&wire));

    char json_builder_buf[128];
    int n = bb_ota_progress_json(json_builder_buf, sizeof(json_builder_buf), via, state, pct);
    TEST_ASSERT_GREATER_THAN(0, n);

    render_eq(&bb_ota_hooks_wire_desc, &wire, json_builder_buf);
}

void test_wire_desc_ota_hooks_render_start(void)
{
    ota_hooks_render_eq_json_builder("push", BB_OTA_PHASE_START, 0, 0);
}

void test_wire_desc_ota_hooks_render_progress(void)
{
    ota_hooks_render_eq_json_builder("pull", BB_OTA_PHASE_PROGRESS, 1, 42);
}

void test_wire_desc_ota_hooks_render_success(void)
{
    ota_hooks_render_eq_json_builder("boot", BB_OTA_PHASE_SUCCESS, 2, 100);
}

void test_wire_desc_ota_hooks_render_fail(void)
{
    ota_hooks_render_eq_json_builder("push", BB_OTA_PHASE_FAIL, 3, 0);
}

void test_wire_desc_ota_hooks_gather_rejects_null(void)
{
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_ota_hooks_gather(NULL));
}

// gather() can only ever produce a valid bb_ota_phase_t (0..3) via
// bb_ota_emit_progress() -- the "unknown" (>3) fallback is reachable only
// through bb_ota_progress_json()'s decoupled raw-int `state` parameter, so
// this case (unlike the 4 phases above) exercises the desc render against a
// hand-built fixture rather than gather() itself.
void test_wire_desc_ota_hooks_render_unknown_fallback(void)
{
    bb_ota_hooks_wire_t snap;
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.via, "push", sizeof(snap.via) - 1);
    strncpy(snap.state, "unknown", sizeof(snap.state) - 1);
    snap.pct = 0;

    char json_builder_buf[128];
    int n = bb_ota_progress_json(json_builder_buf, sizeof(json_builder_buf), "push", 99, 0);
    TEST_ASSERT_GREATER_THAN(0, n);

    render_eq(&bb_ota_hooks_wire_desc, &snap, json_builder_buf);
}

// ---------------------------------------------------------------------------
// 3. bb_health (health.stack) -- initial (low=false) and transition
// (low=true), byte-equal to bb_health_stack_build_json().
// ---------------------------------------------------------------------------

static void health_stack_render_eq_json_builder(const char *task, uint32_t free_bytes, bool low)
{
    bb_health_stack_wire_t snap;
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.task, task, sizeof(snap.task) - 1);
    snap.free_bytes = (int64_t)free_bytes;
    snap.low        = low;

    char json_builder_buf[128];
    int n = bb_health_stack_build_json(json_builder_buf, sizeof(json_builder_buf), task, free_bytes, low);
    TEST_ASSERT_GREATER_THAN(0, n);

    render_eq(&bb_health_stack_wire_desc, &snap, json_builder_buf);
}

void test_wire_desc_health_stack_render_initial_low_false(void)
{
    health_stack_render_eq_json_builder("", 0, false);
}

void test_wire_desc_health_stack_render_transition_low_true(void)
{
    health_stack_render_eq_json_builder("bb_wifi", 128, true);
}

// ---------------------------------------------------------------------------
// 4a. bb_diag (diag.boot) -- present-predicate branches + reboot_history
// wraparound (0/1/8 entries).
//
// B1-1053 PR1 CUTOVER NOTE: unlike the other producer sections in this file
// (still pre-cutover, still diffed against their live bb_json serializer),
// diag.boot's REST GET path has ALREADY moved onto bb_data_render() and its
// bb_json bb_cache serializer (bb_diag_boot_serialize(),
// components/bb_diag/bb_diag_event_common.c) is DELETED -- there is no more
// "old" implementation to diff against. These tests now pin the rendered
// JSON against hand-written literal golden strings instead (same style as
// test_bb_mdns_cache_wire.c), derived from bb_diag_boot_wire_desc's known
// field order/presence-gating (bb_diag_boot_wire.c) -- byte-identical to
// what the retired serializer used to produce for the same fixture (that
// parity was proven by this exact test group before the cutover). PR2's
// other 5 producer sections stay parity-diffed until each is cut over in
// turn.
// ---------------------------------------------------------------------------

static void diag_boot_render_eq(const bb_diag_boot_snap_t *raw, const char *golden)
{
    // Register + populate the diag.boot bb_cache entry so
    // bb_diag_boot_gather() can read it back via bb_cache_get_raw(). Reset
    // first for test isolation (mirrors test_bb_cache_fidelity.c).
    bb_cache_reset_for_test();
    bb_cache_config_t cfg = {
        .key       = BB_DIAG_BOOT_TOPIC,
        .snapshot  = NULL,
        .snap_size = sizeof(bb_diag_boot_snap_t),
        // .serialize left NULL -- this key has no legacy bb_json serializer
        // anymore (B1-1053 PR1); bb_cache_register() accepts that now (see
        // bb_cache.h's field doc). Only bb_cache_get_raw() (via
        // bb_diag_boot_gather()) reads this entry, never
        // bb_cache_serialize_into()/bb_cache_get_serialized().
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_update(&(bb_cache_update_t){ .key = BB_DIAG_BOOT_TOPIC, .snap = raw }));

    bb_diag_boot_wire_t wire;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_diag_boot_gather(&wire));

    render_eq(&bb_diag_boot_wire_desc, &wire, golden);
}

void test_wire_desc_diag_boot_render_no_panic(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason = "power-on",
    };
    diag_boot_render_eq(&snap,
        "{\"reset_reason\":\"power-on\",\"wdt_resets\":0,\"panic\":{\"available\":false},"
        "\"pending_verify\":false,\"rolled_back\":false,"
        "\"reboot_reason\":{\"source\":\"unknown\",\"uptime_s\":0,\"epoch_s\":0},"
        "\"reboot_history\":[]}");
}

void test_wire_desc_diag_boot_render_panic_available(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason      = "panic",
        .wdt_resets        = 3,
        .panic_available   = true,
        .panic_boots_since = 2,
    };
    diag_boot_render_eq(&snap,
        "{\"reset_reason\":\"panic\",\"wdt_resets\":3,"
        "\"panic\":{\"available\":true,\"boots_since\":2},"
        "\"pending_verify\":false,\"rolled_back\":false,"
        "\"reboot_reason\":{\"source\":\"unknown\",\"uptime_s\":0,\"epoch_s\":0},"
        "\"reboot_history\":[]}");
}

void test_wire_desc_diag_boot_render_detail_empty(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason = "power-on",
    };
    diag_boot_render_eq(&snap,
        "{\"reset_reason\":\"power-on\",\"wdt_resets\":0,\"panic\":{\"available\":false},"
        "\"pending_verify\":false,\"rolled_back\":false,"
        "\"reboot_reason\":{\"source\":\"unknown\",\"uptime_s\":0,\"epoch_s\":0},"
        "\"reboot_history\":[]}");
}

void test_wire_desc_diag_boot_render_detail_nonempty(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason    = "software",
        .reboot_src      = (uint8_t)BB_RESET_SRC_EGRESS_TIER3,
        .reboot_detail   = "gw unreachable",
        .reboot_uptime_s = 3600,
    };
    diag_boot_render_eq(&snap,
        "{\"reset_reason\":\"software\",\"wdt_resets\":0,\"panic\":{\"available\":false},"
        "\"pending_verify\":false,\"rolled_back\":false,"
        "\"reboot_reason\":{\"source\":\"egress_tier3\",\"detail\":\"gw unreachable\","
        "\"uptime_s\":3600,\"epoch_s\":0},"
        "\"reboot_history\":[]}");
}

void test_wire_desc_diag_boot_render_age_s_omitted_when_now_invalid(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason    = "software",
        .reboot_src      = (uint8_t)BB_RESET_SRC_API_REBOOT,
        .reboot_epoch_s  = 1735689600U,
        .now_epoch_valid = false,
        .now_epoch_s     = 1735689700U,
    };
    diag_boot_render_eq(&snap,
        "{\"reset_reason\":\"software\",\"wdt_resets\":0,\"panic\":{\"available\":false},"
        "\"pending_verify\":false,\"rolled_back\":false,"
        "\"reboot_reason\":{\"source\":\"api_reboot\",\"uptime_s\":0,\"epoch_s\":1735689600},"
        "\"reboot_history\":[]}");
}

void test_wire_desc_diag_boot_render_age_s_present_when_both_valid(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason    = "software",
        .reboot_src      = (uint8_t)BB_RESET_SRC_API_REBOOT,
        .reboot_epoch_s  = 1735689600U,
        .now_epoch_valid = true,
        .now_epoch_s     = 1735689700U,
    };
    diag_boot_render_eq(&snap,
        "{\"reset_reason\":\"software\",\"wdt_resets\":0,\"panic\":{\"available\":false},"
        "\"pending_verify\":false,\"rolled_back\":false,"
        "\"reboot_reason\":{\"source\":\"api_reboot\",\"uptime_s\":0,\"epoch_s\":1735689600,"
        "\"age_s\":100},"
        "\"reboot_history\":[]}");
}

void test_wire_desc_diag_boot_render_history_empty(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason = "power-on",
    };
    diag_boot_render_eq(&snap,
        "{\"reset_reason\":\"power-on\",\"wdt_resets\":0,\"panic\":{\"available\":false},"
        "\"pending_verify\":false,\"rolled_back\":false,"
        "\"reboot_reason\":{\"source\":\"unknown\",\"uptime_s\":0,\"epoch_s\":0},"
        "\"reboot_history\":[]}");
}

void test_wire_desc_diag_boot_render_history_one_entry(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason = "software",
    };
    snap.reboot_history.head  = 0;
    snap.reboot_history.count = 1;
    snap.reboot_history.entries[0] = (bb_reboot_hist_entry_t){
        .src = (uint8_t)BB_RESET_SRC_API_REBOOT, .epoch_s = 100, .uptime_s = 10,
    };
    diag_boot_render_eq(&snap,
        "{\"reset_reason\":\"software\",\"wdt_resets\":0,\"panic\":{\"available\":false},"
        "\"pending_verify\":false,\"rolled_back\":false,"
        "\"reboot_reason\":{\"source\":\"unknown\",\"uptime_s\":0,\"epoch_s\":0},"
        "\"reboot_history\":[{\"source\":\"api_reboot\",\"epoch_s\":100,\"uptime_s\":10}]}");
}

void test_wire_desc_diag_boot_render_history_eight_entries_wraparound(void)
{
    // Ring at capacity and wrapped: head==3 (oldest slot), count==CAP.
    // Newest push landed at slot (head + count - 1) % CAP == (3+8-1)%8 == 2
    // -- marked OTA_BOOT_DONE below and expected FIRST (newest-first);
    // oldest slot (head==3) marked WIFI_SAFEGUARD, expected LAST.
    bb_diag_boot_snap_t snap = {
        .reset_reason = "software",
    };
    snap.reboot_history.head  = 3;
    snap.reboot_history.count = BB_REBOOT_HISTORY_CAP;
    for (uint8_t i = 0; i < BB_REBOOT_HISTORY_CAP; i++) {
        snap.reboot_history.entries[i] = (bb_reboot_hist_entry_t){
            .src = (uint8_t)BB_RESET_SRC_API_REBOOT, .epoch_s = 1000U + i, .uptime_s = i,
        };
    }
    snap.reboot_history.entries[2].src = (uint8_t)BB_RESET_SRC_OTA_BOOT_DONE;
    snap.reboot_history.entries[3].src = (uint8_t)BB_RESET_SRC_WIFI_SAFEGUARD;
    diag_boot_render_eq(&snap,
        "{\"reset_reason\":\"software\",\"wdt_resets\":0,\"panic\":{\"available\":false},"
        "\"pending_verify\":false,\"rolled_back\":false,"
        "\"reboot_reason\":{\"source\":\"unknown\",\"uptime_s\":0,\"epoch_s\":0},"
        "\"reboot_history\":["
        "{\"source\":\"ota_boot_done\",\"epoch_s\":1002,\"uptime_s\":2},"
        "{\"source\":\"api_reboot\",\"epoch_s\":1001,\"uptime_s\":1},"
        "{\"source\":\"api_reboot\",\"epoch_s\":1000,\"uptime_s\":0},"
        "{\"source\":\"api_reboot\",\"epoch_s\":1007,\"uptime_s\":7},"
        "{\"source\":\"api_reboot\",\"epoch_s\":1006,\"uptime_s\":6},"
        "{\"source\":\"api_reboot\",\"epoch_s\":1005,\"uptime_s\":5},"
        "{\"source\":\"api_reboot\",\"epoch_s\":1004,\"uptime_s\":4},"
        "{\"source\":\"wifi_safeguard\",\"epoch_s\":1003,\"uptime_s\":3}]}");
}

void test_wire_desc_diag_boot_gather_rejects_null(void)
{
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_diag_boot_gather(NULL));
}

// bb_cache_get_raw() propagation path -- BB_DIAG_BOOT_TOPIC never
// registered, so the read-miss BB_ERR_NOT_FOUND must flow straight back out
// of bb_diag_boot_gather() unmodified (the `if (err != BB_OK) return err;`
// early-return branch).
void test_wire_desc_diag_boot_gather_returns_err_when_not_published(void)
{
    bb_cache_reset_for_test();

    bb_diag_boot_wire_t wire;
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_diag_boot_gather(&wire));
}

// age_s_valid's 3rd guard clause (now_epoch_s >= reboot_epoch_s) -- clock
// skew where now precedes the recorded reboot epoch (both fields otherwise
// individually valid) must still omit age_s, distinct from the
// now_epoch_valid==false case above.
void test_wire_desc_diag_boot_render_age_s_omitted_when_clock_skew(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason    = "software",
        .reboot_src      = (uint8_t)BB_RESET_SRC_API_REBOOT,
        .reboot_epoch_s  = 1735689700U,
        .now_epoch_valid = true,
        .now_epoch_s     = 1735689600U, // < reboot_epoch_s
    };
    diag_boot_render_eq(&snap,
        "{\"reset_reason\":\"software\",\"wdt_resets\":0,\"panic\":{\"available\":false},"
        "\"pending_verify\":false,\"rolled_back\":false,"
        "\"reboot_reason\":{\"source\":\"api_reboot\",\"uptime_s\":0,\"epoch_s\":1735689700},"
        "\"reboot_history\":[]}");
}

// Defensive clamp against a corrupted/hand-crafted count field wider than
// BB_REBOOT_HISTORY_CAP, through the gather() path.
void test_wire_desc_diag_boot_render_history_count_clamped_when_out_of_range(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason = "power-on",
    };
    snap.reboot_history.head  = 0;
    snap.reboot_history.count = 200; // out-of-range on purpose
    for (uint8_t i = 0; i < BB_REBOOT_HISTORY_CAP; i++) {
        snap.reboot_history.entries[i] = (bb_reboot_hist_entry_t){
            .src = (uint8_t)BB_RESET_SRC_API_REBOOT, .epoch_s = i, .uptime_s = i,
        };
    }
    diag_boot_render_eq(&snap,
        "{\"reset_reason\":\"power-on\",\"wdt_resets\":0,\"panic\":{\"available\":false},"
        "\"pending_verify\":false,\"rolled_back\":false,"
        "\"reboot_reason\":{\"source\":\"unknown\",\"uptime_s\":0,\"epoch_s\":0},"
        "\"reboot_history\":["
        "{\"source\":\"api_reboot\",\"epoch_s\":7,\"uptime_s\":7},"
        "{\"source\":\"api_reboot\",\"epoch_s\":6,\"uptime_s\":6},"
        "{\"source\":\"api_reboot\",\"epoch_s\":5,\"uptime_s\":5},"
        "{\"source\":\"api_reboot\",\"epoch_s\":4,\"uptime_s\":4},"
        "{\"source\":\"api_reboot\",\"epoch_s\":3,\"uptime_s\":3},"
        "{\"source\":\"api_reboot\",\"epoch_s\":2,\"uptime_s\":2},"
        "{\"source\":\"api_reboot\",\"epoch_s\":1,\"uptime_s\":1},"
        "{\"source\":\"api_reboot\",\"epoch_s\":0,\"uptime_s\":0}]}");
}

// UINT32_MAX wdt_resets -- restores 1:1 traceability with the deleted
// test_bb_diag_boot_serialize_large_wdt_resets (test_bb_diag_event.c).
// wdt_resets is widened to int64_t on the wire (bb_diag_boot_wire_t), so
// UINT32_MAX (4294967295) fits without truncation or sign issues.
void test_wire_desc_diag_boot_render_large_wdt_resets(void)
{
    bb_diag_boot_snap_t snap = {
        .reset_reason = "task_wdt",
        .wdt_resets   = 4294967295U,
    };
    diag_boot_render_eq(&snap,
        "{\"reset_reason\":\"task_wdt\",\"wdt_resets\":4294967295,"
        "\"panic\":{\"available\":false},"
        "\"pending_verify\":false,\"rolled_back\":false,"
        "\"reboot_reason\":{\"source\":\"unknown\",\"uptime_s\":0,\"epoch_s\":0},"
        "\"reboot_history\":[]}");
}

// ---------------------------------------------------------------------------
// 4b. bb_display (health.display) -- present=false/true, byte-equal to
// bb_display_serialize().
// ---------------------------------------------------------------------------

static char *display_info_serialize_to_str(const bb_display_snap_t *snap)
{
    bb_json_t obj = bb_json_obj_new();
    if (!obj) return NULL;
    bb_display_serialize(obj, snap);
    char *str = bb_json_serialize(obj);
    bb_json_free(obj);
    return str;
}

static void display_info_render_eq_current_serializer(const bb_display_snap_t *raw)
{
    char *golden = display_info_serialize_to_str(raw);
    TEST_ASSERT_NOT_NULL(golden);

    bb_cache_reset_for_test();
    bb_cache_config_t cfg = {
        .key       = BB_DISPLAY_INFO_TOPIC,
        .snapshot  = NULL,
        .snap_size = sizeof(bb_display_snap_t),
        .serialize = bb_display_serialize,
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_update(&(bb_cache_update_t){ .key = BB_DISPLAY_INFO_TOPIC, .snap = raw }));

    bb_display_info_wire_t wire;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_display_info_gather(&wire));

    render_eq(&bb_display_info_wire_desc, &wire, golden);
    bb_json_free_str(golden);
}

void test_wire_desc_display_info_render_present_false(void)
{
    bb_display_snap_t snap = { .present = false };
    display_info_render_eq_current_serializer(&snap);
}

void test_wire_desc_display_info_render_present_true(void)
{
    bb_display_snap_t snap = {
        .present = true,
        .width   = 240,
        .height  = 320,
        .enabled = true,
    };
    strncpy(snap.panel, "ili9341", sizeof(snap.panel) - 1);
    display_info_render_eq_current_serializer(&snap);
}

// bb_cache_get_raw() propagation path -- BB_DISPLAY_INFO_TOPIC never
// registered, so the read-miss BB_ERR_NOT_FOUND must flow straight back out
// of bb_display_info_gather() unmodified (the `if (err != BB_OK) return
// err;` early-return branch).
void test_wire_desc_display_info_gather_returns_err_when_not_published(void)
{
    bb_cache_reset_for_test();

    bb_display_info_wire_t wire;
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_display_info_gather(&wire));
}

void test_wire_desc_display_info_gather_rejects_null(void)
{
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_display_info_gather(NULL));
}

// ---------------------------------------------------------------------------
// 4c. bb_ota_check (update.available) -- last_check_ts==0 (omitted) /
// !=0.
//
// B1-1053 PR3 CUTOVER NOTE: like diag.boot's 4a section above,
// update.available's REST GET path has now moved onto bb_data_render() and
// its bb_json bb_cache serializer (bb_ota_check_serialize(),
// components/bb_ota_check/src/bb_ota_check_common.c) is DELETED -- there is
// no more "old" implementation to diff against. These tests now pin the
// rendered JSON against hand-written literal golden strings instead,
// derived from bb_ota_check_wire_desc's known field order/presence-gating
// (bb_ota_check_wire.c) -- byte-identical to what the retired serializer
// used to produce for the same fixture (that parity was proven by this
// exact test group before the cutover).
// ---------------------------------------------------------------------------

static void ota_check_render_eq(const bb_ota_check_snap_t *raw, const char *golden)
{
    // Register + populate the update.available bb_cache entry so
    // bb_ota_check_gather() can read it back via bb_cache_get_raw(). Reset
    // first for test isolation (mirrors diag_boot_render_eq() above).
    bb_cache_reset_for_test();
    bb_cache_config_t cfg = {
        .key       = BB_OTA_CHECK_TOPIC,
        .snapshot  = NULL,
        .snap_size = sizeof(bb_ota_check_snap_t),
        // .serialize left NULL -- this key has no legacy bb_json serializer
        // anymore (B1-1053 PR3); bb_cache_register() accepts that now (see
        // bb_cache.h's field doc). Only bb_cache_get_raw() (via
        // bb_ota_check_gather()) reads this entry, never
        // bb_cache_serialize_into()/bb_cache_get_serialized().
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_cache_register(&cfg));
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_cache_update(&(bb_cache_update_t){ .key = BB_OTA_CHECK_TOPIC, .snap = raw }));

    bb_ota_check_snap_t wire;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_ota_check_gather(&wire));

    render_eq(&bb_ota_check_wire_desc, &wire, golden);
}

void test_wire_desc_ota_check_render_last_check_ts_zero_omitted(void)
{
    bb_ota_check_snap_t snap = { 0 };
    strncpy(snap.current, "1.0.0", sizeof(snap.current) - 1);
    strncpy(snap.latest, "1.0.0", sizeof(snap.latest) - 1);
    strncpy(snap.outcome, "up_to_date", sizeof(snap.outcome) - 1);
    snap.last_check_ok = true;
    snap.enabled        = true;
    snap.last_check_ts  = 0;
    ota_check_render_eq(&snap,
        "{\"current\":\"1.0.0\",\"latest\":\"1.0.0\",\"download_url\":\"\","
        "\"available\":false,\"ts\":0,\"last_check_ok\":true,\"enabled\":true,"
        "\"outcome\":\"up_to_date\"}");
}

void test_wire_desc_ota_check_render_last_check_ts_nonzero_present(void)
{
    bb_ota_check_snap_t snap = { 0 };
    strncpy(snap.current, "1.0.0", sizeof(snap.current) - 1);
    strncpy(snap.latest, "1.1.0", sizeof(snap.latest) - 1);
    strncpy(snap.download_url, "https://example.com/fw.bin", sizeof(snap.download_url) - 1);
    strncpy(snap.outcome, "available", sizeof(snap.outcome) - 1);
    snap.available      = true;
    snap.ts             = 1735689600;
    snap.last_check_ok  = true;
    snap.enabled        = true;
    snap.last_check_ts  = 1735689600;
    ota_check_render_eq(&snap,
        "{\"current\":\"1.0.0\",\"latest\":\"1.1.0\","
        "\"download_url\":\"https://example.com/fw.bin\","
        "\"available\":true,\"ts\":1735689600,\"last_check_ok\":true,\"enabled\":true,"
        "\"outcome\":\"available\",\"last_check_ts\":1735689600}");
}

void test_wire_desc_ota_check_gather_rejects_null(void)
{
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, bb_ota_check_gather(NULL));
}

// bb_cache_get_raw() propagation path -- BB_OTA_CHECK_TOPIC never
// registered, so the read-miss BB_ERR_NOT_FOUND must flow straight back out
// of bb_ota_check_gather() unmodified (the `if (err != BB_OK) return err;`
// early-return branch).
void test_wire_desc_ota_check_gather_returns_err_when_not_published(void)
{
    bb_cache_reset_for_test();

    bb_ota_check_snap_t wire;
    TEST_ASSERT_EQUAL_INT(BB_ERR_NOT_FOUND, bb_ota_check_gather(&wire));
}
