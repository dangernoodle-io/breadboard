// Tests for bb_ws_server_diag -- exercises bb_ws_server_diag_fill() (the
// exact production code path) against bb_ws_server_open_count()'s host
// simulate hooks.
#include "unity.h"
#include "bb_ws_server_diag.h"
#include "bb_ws_server_diag_test.h"
#include "bb_ws_server.h"
#include "bb_ws_server_host.h"

void test_bb_ws_server_diag_fill_null_dst_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(BB_ERR_INVALID_ARG, bb_ws_server_diag_fill(NULL, NULL));
}

void test_bb_ws_server_diag_fill_zero_by_default(void)
{
    bb_ws_server_host_reset_captures();

    bb_ws_server_diag_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_ws_server_diag_fill(&snap, NULL));
    TEST_ASSERT_EQUAL_INT64(0, snap.open_connections);
}

void test_bb_ws_server_diag_fill_reflects_open_count(void)
{
    bb_ws_server_host_reset_captures();
    bb_ws_server_host_simulate_open();
    bb_ws_server_host_simulate_open();

    bb_ws_server_diag_snap_t snap;
    TEST_ASSERT_EQUAL(BB_OK, bb_ws_server_diag_fill(&snap, NULL));
    TEST_ASSERT_EQUAL_INT64(2, snap.open_connections);

    bb_ws_server_host_reset_captures();
}

// Registration fits the shared scratch buffer -- turns the "confirm the
// snapshot fits" requirement into an actual regression test.
void test_bb_ws_server_diag_desc_fits_scratch(void)
{
    bb_diag_section_test_reset();

    bb_diag_section_t section = {
        .name         = "websocket",
        .desc         = "test",
        .snap_desc    = &bb_ws_server_diag_desc,
        .fill         = bb_ws_server_diag_fill,
        .ctx          = NULL,
        .query_keys   = NULL,
        .n_query_keys = 0,
    };
    TEST_ASSERT_EQUAL(BB_OK, bb_diag_register_section(&section));

    bb_diag_section_test_reset();
}

/* ---------------------------------------------------------------------------
 * CONFIG_BB_OPENAPI_RUNTIME_META OFF (this env's default -- undefined) --
 * proves the describe route's 200-response schema is still the UNCHANGED
 * const literal path, byte- and pointer-identical to bb_ws_server_diag_
 * schema (both assigned from the SAME BB_WS_SERVER_DIAG_SCHEMA_LITERAL
 * macro invocation). B1-1059 PR-3 batch 1: config-OFF is a zero-diff no-op.
 * ---------------------------------------------------------------------------*/
void test_bb_ws_server_diag_describe_schema_is_unchanged_const_literal(void)
{
    // The for-test assemble is a documented no-op at this config gate (see
    // bb_ws_server_diag_test.h) -- still exercised here so the
    // compiled-out #else arm of bb_ws_server_diag_assemble_schema_for_test()
    // is covered, not just the accessor.
    TEST_ASSERT_EQUAL(BB_OK, bb_ws_server_diag_assemble_schema_for_test());

    TEST_ASSERT_EQUAL_PTR(bb_ws_server_diag_schema,
                           bb_ws_server_diag_get_describe_schema_for_test());
}
