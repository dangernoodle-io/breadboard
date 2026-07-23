// Host tests for bb_diag_heap_check_wire (GET /api/diag/heap-check emit,
// object-wrapped {"integrity_ok":<bool>} shape). Renders the top-level
// bb_diag_heap_check_wire_desc via bb_serialize_json_render() (the same
// one-shot entry point heap_check_get_handler ultimately drives through
// bb_http_serialize_stream()) and asserts the resulting JSON string exactly,
// byte for byte, matching the pre-migration hand cJSON emitter's output.

#include "unity.h"

#include "../../components/bb_diag_http/bb_diag_heap_check_wire_priv.h"

#include "bb_serialize_json.h"

#include <string.h>

#define RENDER_BUF_BYTES 256

// Renders `integrity_ok` (via the same fill() helper the production handler
// uses) as the top-level {"integrity_ok":...} object and returns the
// NUL-terminated JSON string in `out_buf` (caller-owned, RENDER_BUF_BYTES
// capacity).
static void heap_check_render(bool integrity_ok, char *out_buf)
{
    bb_diag_heap_check_wire_t snap;
    bb_diag_heap_check_wire_fill(&snap, integrity_ok);

    size_t out_len = 0;
    bb_err_t rc = bb_serialize_json_render(&bb_diag_heap_check_wire_desc, &snap,
                                            out_buf, RENDER_BUF_BYTES, &out_len);
    TEST_ASSERT_EQUAL(BB_OK, rc);
}

void test_bb_diag_heap_check_wire_expected_json_true(void)
{
    char buf[RENDER_BUF_BYTES];
    heap_check_render(true, buf);
    TEST_ASSERT_EQUAL_STRING("{\"integrity_ok\":true}", buf);
}

void test_bb_diag_heap_check_wire_expected_json_false(void)
{
    char buf[RENDER_BUF_BYTES];
    heap_check_render(false, buf);
    TEST_ASSERT_EQUAL_STRING("{\"integrity_ok\":false}", buf);
}

void test_bb_diag_heap_check_wire_fill_zero_inits(void)
{
    bb_diag_heap_check_wire_t snap;
    memset(&snap, 0xAA, sizeof(snap));
    bb_diag_heap_check_wire_fill(&snap, true);
    TEST_ASSERT_TRUE(snap.integrity_ok);
}
