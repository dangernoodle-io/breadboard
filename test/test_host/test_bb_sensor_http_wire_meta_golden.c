// test_bb_sensor_http_wire_meta_golden -- B1-1180 PR-2: proves the #if-gated
// co-located bb_serialize_desc_meta_t pattern (B1-1059 PR-2a exemplar,
// test_bb_wifi_http_wire_meta_golden.c) for the three /api/sensors/*
// sections (all descriptors/meta co-located in
// components/bb_sensor_http/bb_sensor_http_wire.c), proving byte-fidelity
// against the hand-authored, on-device (NOT host-gated) literals the
// describe-only routes (bb_sensor_http_describe_routes(), same file) use to
// make GET+PATCH /api/sensors/fan, GET /api/sensors/power, and GET
// /api/sensors/thermal visible to bb_openapi_emit(). Only reachable when
// BB_SERIALIZE_META_HOST is defined (this native env; see platformio.ini).
//
// FAN (B1-1180 PR-2 review fix, HIGH 2): CONFIG_BB_FAN_AUTOFAN forks fan's
// wire shape (6 fields with autofan targets, vs. 2 fields with a flat
// manual duty -- the Kconfig DEFAULT, CONFIG_BB_FAN_AUTOFAN defaults to n)
// -- but this native env force-enables the autofan variant
// (-DCONFIG_BB_FAN_AUTOFAN, platformio.ini). Two-tier fix, so the real
// production descriptor's own meta table is never left untested:
//
//   1. PRODUCTION (always, both response+request goldens): the real
//      `bb_sensor_http_fan_wire_desc`, paired with the production-alias
//      `bb_sensor_http_fan_meta`/`bb_sensor_http_fan_request_meta`
//      (bb_sensor_http_wire.c -- #ifdef CONFIG_BB_FAN_AUTOFAN-selected, same
//      fork as the real descriptor), golden-tested against the real
//      `bb_sensor_http_fan_schema`/`_request_schema`. This is genuine
//      production coverage for whichever variant this build's Kconfig
//      selects as ACTIVE -- not a twin.
//   2. TWIN (dark-branch only, B1-1093): a self-contained
//      `bb_sensor_http_fan_{autofan,manual}_shape_desc` (dummy zero
//      `.offset` fields -- unused by the composer/validator below) covers
//      ONLY the variant that's currently INACTIVE in this build (the one
//      that can't compile as production here, so has no real descriptor to
//      test against) -- `#if defined(CONFIG_BB_FAN_AUTOFAN)` below runs the
//      MANUAL twin (since autofan is active); the opposite build would run
//      the AUTOFAN twin instead. The active variant's own twin is
//      deliberately NOT tested (redundant with #1).
//
// Net: 5 fan tests -- 3 production (validate + GET-response golden +
// PATCH-request golden) + 2 twin (validate + golden, inactive variant
// only) -- so BOTH variants' schema-generation source gets host coverage:
// production via the real descriptor for whichever is active, the twin for
// whichever is dark.
//
// "thermal" is the first exemplar with FOUR sibling nested BB_TYPE_OBJ
// children at the same level (soc/vr/asic/board, each an ordinary
// {present,c} object) -- structurally the SAME one-level-of-nesting shape
// already proven by test_bb_diag_boot_wire_meta_golden.c's "panic"/
// "reboot_reason" children (just more siblings, not more depth), so no
// engine gap: each nested object's own "required" (["present"], since only
// "present" is unconditionally emitted -- "c" is gated by
// thermal_source_c_present()) round-trips byte-exact, same as any other
// per-child `.required` row.
//
// Fidelity findings vs the hand-authored literals: content is
// BYTE-IDENTICAL except the one structural delta every "additionalProperties"
// exemplar in this codebase documents -- none here (every literal was
// authored to already include a matching "additionalProperties":false at
// every level), so this file has NO documented deltas.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "../../../components/bb_sensor_http/bb_sensor_http_wire_priv.h"

#include <string.h>

// ---------------------------------------------------------------------------
// PRODUCTION (always, both variants covered over time via whichever this
// build's Kconfig selects as active) -- the REAL bb_sensor_http_fan_wire_desc,
// paired with the production-alias bb_sensor_http_fan_meta/_request_meta
// (bb_sensor_http_wire.c -- same #ifdef CONFIG_BB_FAN_AUTOFAN fork as the
// real descriptor itself), golden-tested against the real
// bb_sensor_http_fan_schema/_request_schema.
// ---------------------------------------------------------------------------

void test_bb_sensor_http_fan_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_sensor_http_fan_wire_desc, &bb_sensor_http_fan_meta,
                                    err, sizeof err));
}

void test_bb_sensor_http_fan_meta_golden_matches_hand_literal(void)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_sensor_http_fan_wire_desc, &bb_sensor_http_fan_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(bb_sensor_http_fan_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(bb_sensor_http_fan_schema), n);
}

void test_bb_sensor_http_fan_request_meta_golden_matches_hand_literal(void)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_sensor_http_fan_wire_desc, &bb_sensor_http_fan_request_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(bb_sensor_http_fan_request_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(bb_sensor_http_fan_request_schema), n);
}

// ---------------------------------------------------------------------------
// TWIN (dark-branch only, B1-1093) -- covers ONLY the variant that's
// currently INACTIVE in this build (the one that can't compile as
// production here, so has no real descriptor to test against). This native
// env force-enables CONFIG_BB_FAN_AUTOFAN, so autofan is active (covered by
// the production tests above) and manual is dark -- run the MANUAL twin.
// The opposite build (CONFIG_BB_FAN_AUTOFAN unset, the Kconfig DEFAULT)
// would run the AUTOFAN twin instead. Never both -- the active variant's
// own twin would be redundant with the production tests above.
// ---------------------------------------------------------------------------

#if defined(CONFIG_BB_FAN_AUTOFAN)

void test_bb_sensor_http_fan_manual_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_sensor_http_fan_manual_shape_desc, &bb_sensor_http_fan_manual_meta,
                                    err, sizeof err));
}

void test_bb_sensor_http_fan_manual_meta_golden_matches_hand_literal(void)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_sensor_http_fan_manual_shape_desc, &bb_sensor_http_fan_manual_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(bb_sensor_http_fan_manual_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(bb_sensor_http_fan_manual_schema), n);
}

#else

void test_bb_sensor_http_fan_autofan_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_sensor_http_fan_autofan_shape_desc, &bb_sensor_http_fan_autofan_meta,
                                    err, sizeof err));
}

void test_bb_sensor_http_fan_autofan_meta_golden_matches_hand_literal(void)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_sensor_http_fan_autofan_shape_desc, &bb_sensor_http_fan_autofan_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(bb_sensor_http_fan_autofan_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(bb_sensor_http_fan_autofan_schema), n);
}

#endif

void test_bb_sensor_http_power_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_sensor_http_power_wire_desc, &bb_sensor_http_power_meta,
                                    err, sizeof err));
}

void test_bb_sensor_http_power_meta_golden_matches_hand_literal(void)
{
    char   buf[1024];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_sensor_http_power_wire_desc, &bb_sensor_http_power_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(bb_sensor_http_power_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(bb_sensor_http_power_schema), n);
}

void test_bb_sensor_http_thermal_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_sensor_http_thermal_wire_desc, &bb_sensor_http_thermal_meta,
                                    err, sizeof err));
}

void test_bb_sensor_http_thermal_meta_golden_matches_hand_literal(void)
{
    char   buf[1536];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_sensor_http_thermal_wire_desc, &bb_sensor_http_thermal_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(bb_sensor_http_thermal_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(bb_sensor_http_thermal_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
