// Host tests for bb_diag_tasks_get_wire (GET /api/diag/tasks emit).
// Renders the top-level bb_diag_tasks_get_wire_desc via
// bb_serialize_json_render() (the same one-shot entry point
// tasks_get_handler ultimately drives through bb_http_serialize_stream())
// and asserts the resulting JSON string exactly, byte for byte, matching the
// pre-migration hand cJSON emitter's output.

#include "unity.h"

#include "../../components/bb_diag_http/bb_diag_tasks_get_wire_priv.h"

#include "bb_serialize_json.h"

#include <string.h>

#define RENDER_BUF_BYTES 8192

static void fill_default_row(bb_diag_tasks_get_wire_row_t *row, const char *name)
{
    bb_diag_tasks_get_wire_fill_row(row, name, 5, 5, 1024, "running",
                                     false, 0,
                                     false, 0,
                                     false, 0, false,
                                     false, 0, 0, 0, 0);
}

static void render_snap(const bb_diag_tasks_get_wire_row_t *rows, size_t n_rows,
                         uint64_t count, uint64_t capacity, uint64_t dropped,
                         char *out_buf)
{
    bb_diag_tasks_get_wire_t snap;
    bb_diag_tasks_get_wire_fill_snap(&snap, rows, n_rows, count, capacity, dropped);

    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&bb_diag_tasks_get_wire_desc, &snap,
                                            out_buf, RENDER_BUF_BYTES, &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);
}

// ---------------------------------------------------------------------------
// (a) zero tasks -- empty stream, registry unconditional.
// ---------------------------------------------------------------------------

void test_bb_diag_tasks_get_wire_zero_tasks(void)
{
    char buf[RENDER_BUF_BYTES];
    render_snap(NULL, 0, 3, 8, 0, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"tasks\":[],"
        "\"registry\":{\"count\":3,\"capacity\":8,\"dropped\":0}}", buf);
}

// ---------------------------------------------------------------------------
// (b) one task, every optional field absent (nothing self-registered, no
// Kconfig core/runtime present) -- the minimal unconditional 5-field row.
// ---------------------------------------------------------------------------

void test_bb_diag_tasks_get_wire_one_task_no_optional_fields(void)
{
    bb_diag_tasks_get_wire_row_t rows[1];
    fill_default_row(&rows[0], "idle0");

    char buf[RENDER_BUF_BYTES];
    render_snap(rows, 1, 1, 4, 0, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"tasks\":["
        "{\"name\":\"idle0\",\"prio\":5,\"base_prio\":5,\"stack_hwm\":1024,\"state\":\"running\"}"
        "],"
        "\"registry\":{\"count\":1,\"capacity\":4,\"dropped\":0}}", buf);
}

// ---------------------------------------------------------------------------
// (c) one task with ALL 8 present-gated optional fields present --
// core/runtime/registry/sw_wdt all true. Proves the present-predicate
// mechanism works correctly INSIDE a BB_ARR_STREAM row (each of the 4
// row-local predicate fns reads only the row buffer it's handed).
// ---------------------------------------------------------------------------

void test_bb_diag_tasks_get_wire_one_task_all_optional_fields_present(void)
{
    bb_diag_tasks_get_wire_row_t rows[1];
    bb_diag_tasks_get_wire_fill_row(&rows[0], "wifi_task", 10, 9, 2048, "blocked",
                                     true, 1,
                                     true, 123456,
                                     true, 4096, true,
                                     true, 5000, 100, 2, 3000);

    char buf[RENDER_BUF_BYTES];
    render_snap(rows, 1, 1, 4, 0, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"tasks\":["
        "{\"name\":\"wifi_task\",\"prio\":10,\"base_prio\":9,\"stack_hwm\":2048,"
        "\"state\":\"blocked\",\"core\":1,\"runtime\":123456,"
        "\"stack_budget_bytes\":4096,\"wdt_subscribed\":true,"
        "\"sw_wdt_timeout_ms\":5000,\"sw_wdt_last_feed_age_ms\":100,"
        "\"sw_wdt_miss_count\":2,\"sw_wdt_last_miss_age_ms\":3000}"
        "],"
        "\"registry\":{\"count\":1,\"capacity\":4,\"dropped\":0}}", buf);
}

// ---------------------------------------------------------------------------
// (d) multi-row stream render: mixed present/absent across rows in one
// call -- proves BB_ARR_STREAM iterates and emits each row's OWN present
// flags independently (not sticky/shared state across rows).
// ---------------------------------------------------------------------------

void test_bb_diag_tasks_get_wire_multi_row_mixed_presence(void)
{
    bb_diag_tasks_get_wire_row_t rows[3];
    fill_default_row(&rows[0], "idle0");
    bb_diag_tasks_get_wire_fill_row(&rows[1], "wifi_task", 10, 9, 2048, "blocked",
                                     true, 0,
                                     false, 0,
                                     true, 4096, false,
                                     false, 0, 0, 0, 0);
    bb_diag_tasks_get_wire_fill_row(&rows[2], "sw_wdt_task", 6, 6, 512, "ready",
                                     false, 0,
                                     true, 999,
                                     false, 0, false,
                                     true, 1000, 50, 1, 0);

    char buf[RENDER_BUF_BYTES];
    render_snap(rows, 3, 3, 4, 1, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"tasks\":["
        "{\"name\":\"idle0\",\"prio\":5,\"base_prio\":5,\"stack_hwm\":1024,\"state\":\"running\"},"
        "{\"name\":\"wifi_task\",\"prio\":10,\"base_prio\":9,\"stack_hwm\":2048,"
        "\"state\":\"blocked\",\"core\":0,"
        "\"stack_budget_bytes\":4096,\"wdt_subscribed\":false},"
        "{\"name\":\"sw_wdt_task\",\"prio\":6,\"base_prio\":6,\"stack_hwm\":512,"
        "\"state\":\"ready\",\"runtime\":999,"
        "\"sw_wdt_timeout_ms\":1000,\"sw_wdt_last_feed_age_ms\":50,"
        "\"sw_wdt_miss_count\":1,\"sw_wdt_last_miss_age_ms\":0}"
        "],"
        "\"registry\":{\"count\":3,\"capacity\":4,\"dropped\":1}}", buf);
}

// ---------------------------------------------------------------------------
// Row-field-count invariant -- guards BB_DIAG_TASKS_GET_ROW_N_FIELDS
// (bb_diag_tasks_get_wire.c) against drift, same precedent as
// test_bb_diag_sockets_get_wire_row_field_count_matches.
// ---------------------------------------------------------------------------

void test_bb_diag_tasks_get_wire_row_field_count_matches(void)
{
    TEST_ASSERT_EQUAL_UINT16(13, bb_diag_tasks_get_wire_row_n_fields);
}

// ---------------------------------------------------------------------------
// bb_diag_tasks_get_wire_fill_row() zero-inits `row` before populating.
// ---------------------------------------------------------------------------

void test_bb_diag_tasks_get_wire_fill_row_zero_inits(void)
{
    bb_diag_tasks_get_wire_row_t row;
    memset(&row, 0xAA, sizeof(row));
    fill_default_row(&row, "idle0");

    TEST_ASSERT_EQUAL_STRING("idle0", row.name);
    TEST_ASSERT_EQUAL_INT64(5, row.prio);
    TEST_ASSERT_FALSE(row.core_present);
    TEST_ASSERT_FALSE(row.runtime_present);
    TEST_ASSERT_FALSE(row.registry_present);
    TEST_ASSERT_FALSE(row.sw_wdt_present);
}

// ---------------------------------------------------------------------------
// bb_diag_tasks_get_wire_fill_snap() zero-inits `dst` before populating.
// ---------------------------------------------------------------------------

void test_bb_diag_tasks_get_wire_fill_snap_zero_inits(void)
{
    bb_diag_tasks_get_wire_t snap;
    memset(&snap, 0xAA, sizeof(snap));
    bb_diag_tasks_get_wire_fill_snap(&snap, NULL, 0, 7, 9, 2);

    TEST_ASSERT_EQUAL_UINT64(7, snap.registry.count);
    TEST_ASSERT_EQUAL_UINT64(9, snap.registry.capacity);
    TEST_ASSERT_EQUAL_UINT64(2, snap.registry.dropped);
}

// ---------------------------------------------------------------------------
// A NULL/empty `name` argument leaves `row->name` as the empty string
// (defensive -- fill_row must not dereference a NULL name).
// ---------------------------------------------------------------------------

void test_bb_diag_tasks_get_wire_fill_row_null_name(void)
{
    bb_diag_tasks_get_wire_row_t row;
    bb_diag_tasks_get_wire_fill_row(&row, NULL, 1, 1, 1, NULL,
                                     false, 0, false, 0, false, 0, false,
                                     false, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_STRING("", row.name);
    TEST_ASSERT_EQUAL_STRING("?", row.state.ptr);
}

// ---------------------------------------------------------------------------
// All 16 combinations of the 4 present-flags (core_present, runtime_present,
// registry_present, sw_wdt_present) -- full branch coverage of every
// present-predicate fn's true/false path, exercised INSIDE a single-row
// BB_ARR_STREAM render. Asserts each field's key is present/absent in the
// rendered JSON exactly as its bit dictates (substring check, not a full
// literal -- the 16 exact strings would be redundant with (b)/(c)/(d)
// above, which already prove the precise byte layout for the all-absent
// and all-present corners).
// ---------------------------------------------------------------------------

void test_bb_diag_tasks_get_wire_all_16_present_combinations(void)
{
    for (unsigned mask = 0; mask < 16; mask++) {
        bool core_present     = (mask & 0x1) != 0;
        bool runtime_present  = (mask & 0x2) != 0;
        bool registry_present = (mask & 0x4) != 0;
        bool sw_wdt_present   = (mask & 0x8) != 0;

        bb_diag_tasks_get_wire_row_t rows[1];
        bb_diag_tasks_get_wire_fill_row(&rows[0], "t", 1, 1, 1, "running",
                                         core_present, 1,
                                         runtime_present, 2,
                                         registry_present, 3, true,
                                         sw_wdt_present, 4, 5, 6, 7);

        char buf[RENDER_BUF_BYTES];
        render_snap(rows, 1, 0, 0, 0, buf);

        TEST_ASSERT_EQUAL(core_present,     strstr(buf, "\"core\":") != NULL);
        TEST_ASSERT_EQUAL(runtime_present,  strstr(buf, "\"runtime\":") != NULL);
        TEST_ASSERT_EQUAL(registry_present, strstr(buf, "\"stack_budget_bytes\":") != NULL);
        TEST_ASSERT_EQUAL(registry_present, strstr(buf, "\"wdt_subscribed\":") != NULL);
        TEST_ASSERT_EQUAL(sw_wdt_present,   strstr(buf, "\"sw_wdt_timeout_ms\":") != NULL);
        TEST_ASSERT_EQUAL(sw_wdt_present,   strstr(buf, "\"sw_wdt_last_feed_age_ms\":") != NULL);
        TEST_ASSERT_EQUAL(sw_wdt_present,   strstr(buf, "\"sw_wdt_miss_count\":") != NULL);
        TEST_ASSERT_EQUAL(sw_wdt_present,   strstr(buf, "\"sw_wdt_last_miss_age_ms\":") != NULL);
    }
}
