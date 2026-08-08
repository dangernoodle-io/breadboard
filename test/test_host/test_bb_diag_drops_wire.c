// Host tests for bb_diag_drops_wire (GET /api/diag/drops emit, six-counter
// object shape). Renders the top-level bb_diag_drops_wire_desc via
// bb_serialize_json_render() (the same one-shot entry point
// drops_get_handler ultimately drives through bb_http_serialize_stream())
// and asserts the resulting JSON string exactly, byte for byte.

#include "unity.h"

#include "../../components/bb_diag_http/bb_diag_drops_wire_priv.h"

#include "bb_serialize_json.h"

#include <stdint.h>
#include <string.h>

#define RENDER_BUF_BYTES 256

static void drops_render(bb_diag_drops_wire_t *snap, char *out_buf)
{
    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&(bb_serialize_json_render_cfg_t){
        .desc = &bb_diag_drops_wire_desc,
        .snap = snap,
        .buf  = out_buf,
        .cap  = RENDER_BUF_BYTES,
    }, &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);
}

// Each source counter must land in its own, distinct wire field -- a
// transposed field (e.g. udp_dropped landing in event_dropped) is the
// obvious defect this test is designed to catch, so every argument is a
// distinct prime to make a transposition unmistakable in the rendered JSON.
void test_bb_diag_drops_wire_fill_maps_each_field(void)
{
    bb_diag_drops_wire_t snap;
    bb_diag_drops_wire_fill(&snap,
                             /*writer_dropped=*/2, /*udp_dropped=*/3,
                             /*event_dropped=*/5, /*flush_timeouts=*/7,
                             /*render_fail=*/11, /*stale_discard=*/13);

    TEST_ASSERT_EQUAL_INT64(2, snap.writer_dropped);
    TEST_ASSERT_EQUAL_INT64(3, snap.udp_dropped);
    TEST_ASSERT_EQUAL_INT64(5, snap.event_dropped);
    TEST_ASSERT_EQUAL_INT64(7, snap.flush_timeouts);
    TEST_ASSERT_EQUAL_INT64(11, snap.render_fail);
    TEST_ASSERT_EQUAL_INT64(13, snap.stale_discard);
}

void test_bb_diag_drops_wire_expected_json(void)
{
    bb_diag_drops_wire_t snap;
    bb_diag_drops_wire_fill(&snap, 2, 3, 5, 7, 11, 13);

    char buf[RENDER_BUF_BYTES];
    drops_render(&snap, buf);

    TEST_ASSERT_EQUAL_STRING(
        "{\"writer_dropped\":2,\"udp_dropped\":3,\"event_dropped\":5,"
        "\"flush_timeouts\":7,\"render_fail\":11,\"stale_discard\":13}",
        buf);
}

// Boundary value: every source counter is uint32_t/size_t and wraps rather
// than saturates (see bb_diag_drops_wire_priv.h's doc comment), so a counter
// sitting at UINT32_MAX right before wrap is exactly what an operator hits
// mid-incident. bb_diag_drops_wire_fill() widens each into an int64_t wire
// field via an explicit cast -- this proves that widening renders a positive
// 4294967295, not a negative number, rather than merely reasoning about it.
void test_bb_diag_drops_wire_expected_json_uint32_max(void)
{
    bb_diag_drops_wire_t snap;
    bb_diag_drops_wire_fill(&snap, UINT32_MAX, UINT32_MAX, UINT32_MAX,
                             UINT32_MAX, UINT32_MAX, UINT32_MAX);

    char buf[RENDER_BUF_BYTES];
    drops_render(&snap, buf);

    TEST_ASSERT_EQUAL_STRING(
        "{\"writer_dropped\":4294967295,\"udp_dropped\":4294967295,"
        "\"event_dropped\":4294967295,\"flush_timeouts\":4294967295,"
        "\"render_fail\":4294967295,\"stale_discard\":4294967295}",
        buf);
}

void test_bb_diag_drops_wire_fill_zero_inits(void)
{
    bb_diag_drops_wire_t snap;
    memset(&snap, 0xAA, sizeof(snap));
    bb_diag_drops_wire_fill(&snap, 0, 0, 0, 0, 0, 0);

    TEST_ASSERT_EQUAL_INT64(0, snap.writer_dropped);
    TEST_ASSERT_EQUAL_INT64(0, snap.udp_dropped);
    TEST_ASSERT_EQUAL_INT64(0, snap.event_dropped);
    TEST_ASSERT_EQUAL_INT64(0, snap.flush_timeouts);
    TEST_ASSERT_EQUAL_INT64(0, snap.render_fail);
    TEST_ASSERT_EQUAL_INT64(0, snap.stale_discard);
}
