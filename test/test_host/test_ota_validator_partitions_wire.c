// Host tests for bb_ota_validator_partitions_wire (GET /api/update/partitions
// emit, object-wrapped {"partitions":[...]} shape). Renders the top-level
// bb_ota_validator_partitions_wire_desc via bb_serialize_json_render() (the
// same one-shot entry point every other object-shaped route in this
// component ultimately drives through bb_http_serialize_stream()) and
// asserts the resulting JSON string exactly, byte for byte.

#include "unity.h"

#include "../../components/bb_ota_validator/bb_ota_validator_partitions_wire_priv.h"

#include "bb_serialize_json.h"

#include <stdio.h>
#include <string.h>

#define RENDER_BUF_BYTES 4096

// Renders `rows` (via the same copy_rows() helper the production fill fn
// uses) as the top-level {"partitions":[...]} object and returns the
// NUL-terminated JSON string in `out_buf` (caller-owned, RENDER_BUF_BYTES
// capacity).
static void partitions_render(const bb_ota_validator_partition_src_t *rows, size_t count,
                               char *out_buf)
{
    bb_ota_validator_partitions_wire_t snap;
    memset(&snap, 0, sizeof(snap));
    bb_ota_validator_partitions_wire_copy_rows(snap.partitions_items, rows, count);
    snap.partitions.items = snap.partitions_items;
    snap.partitions.count = count;

    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&bb_ota_validator_partitions_wire_desc, &snap,
                                            out_buf, RENDER_BUF_BYTES, &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);
}

// ---------------------------------------------------------------------------
// 1. Zero partitions -> exactly {"partitions":[]}.
// ---------------------------------------------------------------------------

void test_ota_validator_partitions_wire_expected_json_zero_rows(void)
{
    char buf[RENDER_BUF_BYTES];
    partitions_render(NULL, 0, buf);
    TEST_ASSERT_EQUAL_STRING("{\"partitions\":[]}", buf);
}

// ---------------------------------------------------------------------------
// 2. One partition (known values).
// ---------------------------------------------------------------------------

void test_ota_validator_partitions_wire_expected_json_one_row(void)
{
    bb_ota_validator_partition_src_t rows[1] = {
        { .label = "factory", .address = 0x10000, .size = 0x100000,
          .running = true, .state = "valid" },
    };

    char buf[RENDER_BUF_BYTES];
    partitions_render(rows, 1, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"partitions\":[{\"label\":\"factory\",\"address\":65536,\"size\":1048576,"
        "\"running\":true,\"state\":\"valid\"}]}", buf);
}

// ---------------------------------------------------------------------------
// 3. Max-cap (BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP identical rows).
// ---------------------------------------------------------------------------

void test_ota_validator_partitions_wire_expected_json_max_cap(void)
{
    bb_ota_validator_partition_src_t rows[BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP];
    for (int i = 0; i < BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP; i++) {
        rows[i] = (bb_ota_validator_partition_src_t){
            .label = "ota_0", .address = 0x20000, .size = 0x180000,
            .running = false, .state = "new",
        };
    }

    char buf[RENDER_BUF_BYTES];
    partitions_render(rows, BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP, buf);

    char expected[RENDER_BUF_BYTES];
    size_t off = 0;
    off += (size_t)snprintf(expected + off, sizeof(expected) - off, "{\"partitions\":[");
    for (int i = 0; i < BB_OTA_VALIDATOR_PARTITIONS_ROW_CAP; i++) {
        if (i > 0) expected[off++] = ',';
        off += (size_t)snprintf(expected + off, sizeof(expected) - off,
                                 "{\"label\":\"ota_0\",\"address\":131072,\"size\":1572864,"
                                 "\"running\":false,\"state\":\"new\"}");
    }
    off += (size_t)snprintf(expected + off, sizeof(expected) - off, "]}");

    TEST_ASSERT_EQUAL_STRING(expected, buf);
}

// ---------------------------------------------------------------------------
// 4. All-states: one row per ota_state_str value -- exercises the
// BB_TYPE_STR_N borrowed-pointer path with distinct static strings.
// ---------------------------------------------------------------------------

void test_ota_validator_partitions_wire_expected_json_all_states(void)
{
    bb_ota_validator_partition_src_t rows[6] = {
        { .label = "a", .address = 0, .size = 0, .running = false, .state = "new" },
        { .label = "b", .address = 0, .size = 0, .running = false, .state = "pending_verify" },
        { .label = "c", .address = 0, .size = 0, .running = true,  .state = "valid" },
        { .label = "d", .address = 0, .size = 0, .running = false, .state = "invalid" },
        { .label = "e", .address = 0, .size = 0, .running = false, .state = "aborted" },
        { .label = "f", .address = 0, .size = 0, .running = false, .state = "undefined" },
    };

    char buf[RENDER_BUF_BYTES];
    partitions_render(rows, 6, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"partitions\":["
        "{\"label\":\"a\",\"address\":0,\"size\":0,\"running\":false,\"state\":\"new\"},"
        "{\"label\":\"b\",\"address\":0,\"size\":0,\"running\":false,\"state\":\"pending_verify\"},"
        "{\"label\":\"c\",\"address\":0,\"size\":0,\"running\":true,\"state\":\"valid\"},"
        "{\"label\":\"d\",\"address\":0,\"size\":0,\"running\":false,\"state\":\"invalid\"},"
        "{\"label\":\"e\",\"address\":0,\"size\":0,\"running\":false,\"state\":\"aborted\"},"
        "{\"label\":\"f\",\"address\":0,\"size\":0,\"running\":false,\"state\":\"undefined\"}"
        "]}", buf);
}

// ---------------------------------------------------------------------------
// 5. Escaped label (quote + backslash) -- defensive.
// ---------------------------------------------------------------------------

void test_ota_validator_partitions_wire_expected_json_escaped_label(void)
{
    bb_ota_validator_partition_src_t rows[1] = {
        { .label = "Weird\"Label\\Name", .address = 1, .size = 2,
          .running = false, .state = "valid" },
    };

    char buf[RENDER_BUF_BYTES];
    partitions_render(rows, 1, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"partitions\":[{\"label\":\"Weird\\\"Label\\\\Name\",\"address\":1,\"size\":2,"
        "\"running\":false,\"state\":\"valid\"}]}", buf);
}

// ---------------------------------------------------------------------------
// 6. bb_ota_validator_partitions_wire_copy_rows() -- NULL label/state
// defensive fallback (never dereferences a NULL borrowed pointer).
// ---------------------------------------------------------------------------

void test_ota_validator_partitions_wire_copy_rows_null_label_and_state(void)
{
    bb_ota_validator_partition_src_t rows[1] = {
        { .label = NULL, .address = 0, .size = 0, .running = false, .state = NULL },
    };

    char buf[RENDER_BUF_BYTES];
    partitions_render(rows, 1, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"partitions\":[{\"label\":\"\",\"address\":0,\"size\":0,"
        "\"running\":false,\"state\":\"\"}]}", buf);
}

// ---------------------------------------------------------------------------
// 7. Row-field-count invariant -- guards
// BB_OTA_VALIDATOR_PARTITION_ROW_N_FIELDS (bb_ota_validator_partitions_wire.c)
// against drift, same precedent as test_wifi_scan_wire_row_field_count_matches.
// ---------------------------------------------------------------------------

void test_ota_validator_partitions_wire_row_field_count_matches(void)
{
    TEST_ASSERT_EQUAL_UINT16(5, bb_ota_validator_partition_wire_n_fields);
}
