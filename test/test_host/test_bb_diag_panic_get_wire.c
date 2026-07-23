// Host tests for bb_diag_panic_get_wire (GET /api/diag/panic emit). Renders
// the top-level bb_diag_panic_get_wire_desc via bb_serialize_json_render()
// (the same one-shot entry point panic_get_handler ultimately drives
// through bb_http_serialize_stream()) and asserts the resulting JSON string
// exactly, byte for byte, matching the pre-migration hand cJSON emitter's
// output for each present-condition combination documented in
// bb_diag_panic_get_wire_priv.h.

#include "unity.h"

#include "../../components/bb_diag_http/bb_diag_panic_get_wire_priv.h"

#include "bb_serialize_json.h"

#include <string.h>

#define RENDER_BUF_BYTES 1024

static void panic_render(const bb_diag_panic_get_wire_t *snap, char *out_buf)
{
    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&bb_diag_panic_get_wire_desc, snap,
                                            out_buf, RENDER_BUF_BYTES, &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);
}

// (a) nothing available at all -- only "available":false.
void test_bb_diag_panic_get_wire_nothing_available(void)
{
    bb_diag_panic_get_wire_t snap;
    bb_diag_panic_get_wire_fill(&snap, false, false, 0, "unknown", false, "", NULL);

    char buf[RENDER_BUF_BYTES];
    panic_render(&snap, buf);
    TEST_ASSERT_EQUAL_STRING("{\"available\":false}", buf);
}

// (b) available only -- no captured log tail.
void test_bb_diag_panic_get_wire_available_only(void)
{
    bb_diag_panic_get_wire_t snap;
    bb_diag_panic_get_wire_fill(&snap, true, false, 3, "panic", false, "", NULL);

    char buf[RENDER_BUF_BYTES];
    panic_render(&snap, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"available\":true,\"boots_since\":3,\"reset_reason\":\"panic\"}", buf);
}

// (c) available + log_tail captured.
void test_bb_diag_panic_get_wire_available_with_log_tail(void)
{
    bb_diag_panic_get_wire_t snap;
    bb_diag_panic_get_wire_fill(&snap, true, false, 0, "task_wdt", true, "boom", NULL);

    char buf[RENDER_BUF_BYTES];
    panic_render(&snap, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"available\":true,\"boots_since\":0,\"reset_reason\":\"task_wdt\","
        "\"log_tail\":\"boom\"}", buf);
}

// (d) coredump full, including a multi-element backtrace, panic_reason, and
// app_sha256.
void test_bb_diag_panic_get_wire_coredump_full(void)
{
    bb_diag_panic_summary_t summary;
    memset(&summary, 0, sizeof(summary));
    strncpy(summary.task_name, "main", sizeof(summary.task_name) - 1);
    summary.exc_pc    = 0x40080000;
    summary.exc_cause = 6;
    summary.bt_count  = 3;
    summary.bt_addrs[0] = 0x40080100;
    summary.bt_addrs[1] = 0x40080200;
    summary.bt_addrs[2] = 0x40080300;
    strncpy(summary.panic_reason, "InstrFetchProhibited", sizeof(summary.panic_reason) - 1);
    strncpy(summary.app_sha256, "abcdef0123456789", sizeof(summary.app_sha256) - 1);

    bb_diag_panic_get_wire_t snap;
    bb_diag_panic_get_wire_fill(&snap, true, true, 1, "panic", false, "", &summary);

    char buf[RENDER_BUF_BYTES];
    panic_render(&snap, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"available\":true,\"boots_since\":1,\"reset_reason\":\"panic\","
        "\"task\":\"main\",\"exc_pc\":1074266112,\"exc_cause\":6,"
        "\"backtrace\":[1074266368,1074266624,1074266880],"
        "\"panic_reason\":\"InstrFetchProhibited\",\"app_sha256\":\"abcdef0123456789\"}",
        buf);
}

// (e) coredump present but with EMPTY panic_reason/app_sha256 and an EMPTY
// backtrace ([]) -- backtrace is emitted as [] when bt_count is 0, not
// additionally gated on count > 0; panic_reason/app_sha256 are omitted
// (not emitted as "") when their source strings are empty.
void test_bb_diag_panic_get_wire_coredump_empty_optional_fields(void)
{
    bb_diag_panic_summary_t summary;
    memset(&summary, 0, sizeof(summary));
    strncpy(summary.task_name, "IDLE0", sizeof(summary.task_name) - 1);
    summary.exc_pc    = 0;
    summary.exc_cause = 0;
    summary.bt_count  = 0;
    // panic_reason/app_sha256 left empty (zeroed).

    bb_diag_panic_get_wire_t snap;
    bb_diag_panic_get_wire_fill(&snap, false, true, 2, "unknown", false, "", &summary);

    char buf[RENDER_BUF_BYTES];
    panic_render(&snap, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"available\":false,\"boots_since\":2,"
        "\"task\":\"IDLE0\",\"exc_pc\":0,\"exc_cause\":0,\"backtrace\":[]}", buf);
}

// coredump_avail true but the get() call itself failed (summary == NULL) --
// boots_since is still emitted (gated on the RAW coredump_avail flag), but
// none of the coredump-derived fields are (gated on coredump_fields_present,
// which requires a successfully-fetched summary) -- see
// bb_diag_panic_get_wire_priv.h's fidelity note.
void test_bb_diag_panic_get_wire_coredump_avail_but_get_failed(void)
{
    bb_diag_panic_get_wire_t snap;
    bb_diag_panic_get_wire_fill(&snap, false, true, 5, "unknown", false, "", NULL);

    char buf[RENDER_BUF_BYTES];
    panic_render(&snap, buf);
    TEST_ASSERT_EQUAL_STRING("{\"available\":false,\"boots_since\":5}", buf);
}

// (f) available + coredump + log_tail all together: combines available=true,
// coredump_avail=true (with full summary), and log_tail_ok=true (non-empty log_tail).
void test_bb_diag_panic_get_wire_available_coredump_and_logtail(void)
{
    bb_diag_panic_summary_t summary;
    memset(&summary, 0, sizeof(summary));
    strncpy(summary.task_name, "main", sizeof(summary.task_name) - 1);
    summary.exc_pc    = 0x40080000;
    summary.exc_cause = 6;
    summary.bt_count  = 3;
    summary.bt_addrs[0] = 0x40080100;
    summary.bt_addrs[1] = 0x40080200;
    summary.bt_addrs[2] = 0x40080300;
    strncpy(summary.panic_reason, "InstrFetchProhibited", sizeof(summary.panic_reason) - 1);
    strncpy(summary.app_sha256, "abcdef0123456789", sizeof(summary.app_sha256) - 1);

    bb_diag_panic_get_wire_t snap;
    bb_diag_panic_get_wire_fill(&snap, true, true, 1, "panic", true, "boom", &summary);

    char buf[RENDER_BUF_BYTES];
    panic_render(&snap, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"available\":true,\"boots_since\":1,\"reset_reason\":\"panic\","
        "\"log_tail\":\"boom\",\"task\":\"main\",\"exc_pc\":1074266112,\"exc_cause\":6,"
        "\"backtrace\":[1074266368,1074266624,1074266880],"
        "\"panic_reason\":\"InstrFetchProhibited\",\"app_sha256\":\"abcdef0123456789\"}",
        buf);
}

void test_bb_diag_panic_get_wire_fill_zero_inits(void)
{
    bb_diag_panic_get_wire_t snap;
    memset(&snap, 0xAA, sizeof(snap));
    bb_diag_panic_get_wire_fill(&snap, true, false, 0, "panic", false, "", NULL);
    TEST_ASSERT_TRUE(snap.available);
    TEST_ASSERT_FALSE(snap.coredump_avail);
    TEST_ASSERT_FALSE(snap.coredump_fields_present);
    TEST_ASSERT_FALSE(snap.panic_reason_present);
    TEST_ASSERT_FALSE(snap.app_sha256_present);
    TEST_ASSERT_EQUAL_UINT32(0, snap.backtrace.count);
}

// NULL `reset_reason` -- fill()'s `if (reset_reason)` guard's false direction
// (a real caller always passes a literal, but fill() is a directly host-
// testable pure function and must not crash on NULL; output is otherwise
// identical to the "nothing available" case since reset_reason isn't
// present-gated in when available is false).
void test_bb_diag_panic_get_wire_null_reset_reason(void)
{
    bb_diag_panic_get_wire_t snap;
    bb_diag_panic_get_wire_fill(&snap, false, false, 0, NULL, false, "", NULL);

    char buf[RENDER_BUF_BYTES];
    panic_render(&snap, buf);
    TEST_ASSERT_EQUAL_STRING("{\"available\":false}", buf);
}

// log_tail_present true (available && log_tail_ok) but a NULL `log_tail`
// buffer -- fill()'s `if (dst->log_tail_present && log_tail)` guard's false
// direction on the second operand (a real caller always passes a valid
// buffer, but fill() must not crash on NULL); "log_tail" is still emitted
// (present-gated on the precomputed flag alone) but stays empty since the
// copy is skipped.
void test_bb_diag_panic_get_wire_log_tail_present_but_null_buffer(void)
{
    bb_diag_panic_get_wire_t snap;
    bb_diag_panic_get_wire_fill(&snap, true, false, 0, "panic", true, NULL, NULL);

    char buf[RENDER_BUF_BYTES];
    panic_render(&snap, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"available\":true,\"boots_since\":0,\"reset_reason\":\"panic\","
        "\"log_tail\":\"\"}", buf);
}

// bt_count exceeding BB_DIAG_PANIC_BACKTRACE_MAX -- fill()'s clamp guard
// (`if (n > BB_DIAG_PANIC_BACKTRACE_MAX) n = BB_DIAG_PANIC_BACKTRACE_MAX`)
// true direction, protecting against a corrupt/malformed coredump summary
// reporting more backtrace frames than the fixed materialize buffer holds.
void test_bb_diag_panic_get_wire_coredump_backtrace_count_clamped(void)
{
    bb_diag_panic_summary_t summary;
    memset(&summary, 0, sizeof(summary));
    strncpy(summary.task_name, "main", sizeof(summary.task_name) - 1);
    summary.bt_count = BB_DIAG_PANIC_BACKTRACE_MAX + 4; // corrupt: exceeds array capacity
    for (uint32_t i = 0; i < BB_DIAG_PANIC_BACKTRACE_MAX; i++) {
        summary.bt_addrs[i] = 0x40080000 + i;
    }

    bb_diag_panic_get_wire_t snap;
    bb_diag_panic_get_wire_fill(&snap, true, true, 0, "panic", false, "", &summary);

    TEST_ASSERT_EQUAL_UINT32(BB_DIAG_PANIC_BACKTRACE_MAX, snap.backtrace.count);
}
