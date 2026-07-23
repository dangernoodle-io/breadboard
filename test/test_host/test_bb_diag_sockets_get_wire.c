// Host tests for bb_diag_sockets_get_wire (GET /api/diag/sockets emit).
// Renders the top-level bb_diag_sockets_get_wire_desc via
// bb_serialize_json_render() (the same one-shot entry point
// sockets_get_handler ultimately drives through bb_http_serialize_stream())
// and asserts the resulting JSON string exactly, byte for byte, matching the
// pre-migration hand cJSON emitter's output.

#include "unity.h"

#include "../../components/bb_diag_http/bb_diag_sockets_get_wire_priv.h"

#include "bb_serialize_json.h"

#include <stdio.h>
#include <string.h>

#define RENDER_BUF_BYTES 8192

static void sockets_render(uint32_t lwip_max_sockets, uint32_t in_use,
                            const uint32_t by_state[BB_DIAG_SOCKETS_STATE_COUNT],
                            const bb_diag_sockets_pcb_src_t *rows, size_t n_rows,
                            char *out_buf)
{
    bb_diag_sockets_get_wire_t snap;
    bb_diag_sockets_get_wire_fill(&snap, lwip_max_sockets, in_use, by_state, rows, n_rows);

    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&bb_diag_sockets_get_wire_desc, &snap,
                                            out_buf, RENDER_BUF_BYTES, &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);
}

// ---------------------------------------------------------------------------
// (a) zero PCBs, all-zero by_state.
// ---------------------------------------------------------------------------

void test_bb_diag_sockets_get_wire_zero_pcbs(void)
{
    uint32_t by_state[BB_DIAG_SOCKETS_STATE_COUNT] = {0};

    char buf[RENDER_BUF_BYTES];
    sockets_render(16, 0, by_state, NULL, 0, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"lwip_max_sockets\":16,\"in_use\":0,"
        "\"by_state\":{\"CLOSED\":0,\"LISTEN\":0,\"SYN_SENT\":0,\"SYN_RCVD\":0,"
        "\"ESTABLISHED\":0,\"FIN_WAIT_1\":0,\"FIN_WAIT_2\":0,\"CLOSE_WAIT\":0,"
        "\"CLOSING\":0,\"LAST_ACK\":0,\"TIME_WAIT\":0},"
        "\"pcbs\":[]}", buf);
}

// ---------------------------------------------------------------------------
// (b) N PCBs with distinct states.
// ---------------------------------------------------------------------------

void test_bb_diag_sockets_get_wire_n_pcbs_distinct_states(void)
{
    uint32_t by_state[BB_DIAG_SOCKETS_STATE_COUNT] = {0};
    by_state[4] = 2;  // ESTABLISHED
    by_state[7] = 1;  // CLOSE_WAIT

    bb_diag_sockets_pcb_src_t rows[3] = {
        { .local_port = 80,   .remote_port = 51000, .remote_ip = "192.168.1.10", .state_idx = 4 },
        { .local_port = 443,  .remote_port = 51001, .remote_ip = "192.168.1.11", .state_idx = 4 },
        { .local_port = 8080, .remote_port = 51002, .remote_ip = "192.168.1.12", .state_idx = 7 },
    };

    char buf[RENDER_BUF_BYTES];
    sockets_render(16, 3, by_state, rows, 3, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"lwip_max_sockets\":16,\"in_use\":3,"
        "\"by_state\":{\"CLOSED\":0,\"LISTEN\":0,\"SYN_SENT\":0,\"SYN_RCVD\":0,"
        "\"ESTABLISHED\":2,\"FIN_WAIT_1\":0,\"FIN_WAIT_2\":0,\"CLOSE_WAIT\":1,"
        "\"CLOSING\":0,\"LAST_ACK\":0,\"TIME_WAIT\":0},"
        "\"pcbs\":["
        "{\"local_port\":80,\"remote_ip\":\"192.168.1.10\",\"remote_port\":51000,\"state\":\"ESTABLISHED\"},"
        "{\"local_port\":443,\"remote_ip\":\"192.168.1.11\",\"remote_port\":51001,\"state\":\"ESTABLISHED\"},"
        "{\"local_port\":8080,\"remote_ip\":\"192.168.1.12\",\"remote_port\":51002,\"state\":\"CLOSE_WAIT\"}"
        "]}", buf);
}

// ---------------------------------------------------------------------------
// (c) all 11 states nonzero.
// ---------------------------------------------------------------------------

void test_bb_diag_sockets_get_wire_all_states_nonzero(void)
{
    uint32_t by_state[BB_DIAG_SOCKETS_STATE_COUNT];
    for (uint32_t i = 0; i < BB_DIAG_SOCKETS_STATE_COUNT; i++) {
        by_state[i] = i + 1;
    }

    char buf[RENDER_BUF_BYTES];
    sockets_render(16, 0, by_state, NULL, 0, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"lwip_max_sockets\":16,\"in_use\":0,"
        "\"by_state\":{\"CLOSED\":1,\"LISTEN\":2,\"SYN_SENT\":3,\"SYN_RCVD\":4,"
        "\"ESTABLISHED\":5,\"FIN_WAIT_1\":6,\"FIN_WAIT_2\":7,\"CLOSE_WAIT\":8,"
        "\"CLOSING\":9,\"LAST_ACK\":10,\"TIME_WAIT\":11},"
        "\"pcbs\":[]}", buf);
}

// ---------------------------------------------------------------------------
// (d) pcbs at cap (BB_DIAG_SOCKETS_ROW_CAP identical rows).
// ---------------------------------------------------------------------------

void test_bb_diag_sockets_get_wire_pcbs_at_cap(void)
{
    uint32_t by_state[BB_DIAG_SOCKETS_STATE_COUNT] = {0};
    by_state[4] = BB_DIAG_SOCKETS_ROW_CAP;

    bb_diag_sockets_pcb_src_t rows[BB_DIAG_SOCKETS_ROW_CAP];
    for (int i = 0; i < BB_DIAG_SOCKETS_ROW_CAP; i++) {
        rows[i] = (bb_diag_sockets_pcb_src_t){
            .local_port = 80, .remote_port = 51000, .remote_ip = "10.0.0.1", .state_idx = 4,
        };
    }

    char buf[RENDER_BUF_BYTES];
    sockets_render(BB_DIAG_SOCKETS_ROW_CAP, BB_DIAG_SOCKETS_ROW_CAP, by_state,
                   rows, BB_DIAG_SOCKETS_ROW_CAP, buf);

    char expected[RENDER_BUF_BYTES];
    size_t off = 0;
    off += (size_t)snprintf(expected + off, sizeof(expected) - off,
        "{\"lwip_max_sockets\":%d,\"in_use\":%d,"
        "\"by_state\":{\"CLOSED\":0,\"LISTEN\":0,\"SYN_SENT\":0,\"SYN_RCVD\":0,"
        "\"ESTABLISHED\":%d,\"FIN_WAIT_1\":0,\"FIN_WAIT_2\":0,\"CLOSE_WAIT\":0,"
        "\"CLOSING\":0,\"LAST_ACK\":0,\"TIME_WAIT\":0},"
        "\"pcbs\":[",
        BB_DIAG_SOCKETS_ROW_CAP, BB_DIAG_SOCKETS_ROW_CAP, BB_DIAG_SOCKETS_ROW_CAP);
    for (int i = 0; i < BB_DIAG_SOCKETS_ROW_CAP; i++) {
        if (i > 0) expected[off++] = ',';
        off += (size_t)snprintf(expected + off, sizeof(expected) - off,
            "{\"local_port\":80,\"remote_ip\":\"10.0.0.1\",\"remote_port\":51000,"
            "\"state\":\"ESTABLISHED\"}");
    }
    off += (size_t)snprintf(expected + off, sizeof(expected) - off, "]}");

    TEST_ASSERT_EQUAL_STRING(expected, buf);
}

// ---------------------------------------------------------------------------
// bb_diag_sockets_get_wire_copy_rows() -- out-of-range state_idx falls back
// to "UNKNOWN" (defensive, mirrors the pre-migration handler's guard).
// ---------------------------------------------------------------------------

void test_bb_diag_sockets_get_wire_copy_rows_unknown_state_fallback(void)
{
    uint32_t by_state[BB_DIAG_SOCKETS_STATE_COUNT] = {0};
    bb_diag_sockets_pcb_src_t rows[1] = {
        { .local_port = 1, .remote_port = 2, .remote_ip = "1.2.3.4",
          .state_idx = BB_DIAG_SOCKETS_STATE_COUNT + 5 },
    };

    char buf[RENDER_BUF_BYTES];
    sockets_render(16, 1, by_state, rows, 1, buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"lwip_max_sockets\":16,\"in_use\":1,"
        "\"by_state\":{\"CLOSED\":0,\"LISTEN\":0,\"SYN_SENT\":0,\"SYN_RCVD\":0,"
        "\"ESTABLISHED\":0,\"FIN_WAIT_1\":0,\"FIN_WAIT_2\":0,\"CLOSE_WAIT\":0,"
        "\"CLOSING\":0,\"LAST_ACK\":0,\"TIME_WAIT\":0},"
        "\"pcbs\":[{\"local_port\":1,\"remote_ip\":\"1.2.3.4\",\"remote_port\":2,"
        "\"state\":\"UNKNOWN\"}]}", buf);
}

// ---------------------------------------------------------------------------
// Row-field-count invariant -- guards BB_DIAG_SOCKETS_PCB_ROW_N_FIELDS
// (bb_diag_sockets_get_wire.c) against drift, same precedent as
// test_ota_validator_partitions_wire_row_field_count_matches.
// ---------------------------------------------------------------------------

void test_bb_diag_sockets_get_wire_row_field_count_matches(void)
{
    TEST_ASSERT_EQUAL_UINT16(4, bb_diag_sockets_pcb_wire_n_fields);
}

// ---------------------------------------------------------------------------
// bb_diag_sockets_get_wire_fill() zero-inits `dst` before populating.
// ---------------------------------------------------------------------------

void test_bb_diag_sockets_get_wire_fill_zero_inits(void)
{
    uint32_t by_state[BB_DIAG_SOCKETS_STATE_COUNT] = {0};

    bb_diag_sockets_get_wire_t snap;
    memset(&snap, 0xAA, sizeof(snap));
    bb_diag_sockets_get_wire_fill(&snap, 16, 0, by_state, NULL, 0);

    TEST_ASSERT_EQUAL_INT64(16, snap.lwip_max_sockets);
    TEST_ASSERT_EQUAL_INT64(0, snap.in_use);
    TEST_ASSERT_EQUAL_INT64(0, snap.by_state.closed);
    TEST_ASSERT_EQUAL_INT64(0, snap.by_state.time_wait);
    TEST_ASSERT_EQUAL_UINT32(0, snap.pcbs.count);
}
