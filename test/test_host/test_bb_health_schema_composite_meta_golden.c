// test_bb_health_schema_composite_meta_golden -- B1-1181b: proves the
// /api/health COMPOSITE schema assembly (bb_health_assemble_schema(),
// components/bb_health/bb_health_schema_priv.h) correctly splices real
// producer sections' schema_props in registration order, where each
// section's schema_props is itself reconstructed from its co-located
// bb_serialize_desc_meta_t (B1-1059 PR-2b-i-3) via
// bb_serialize_meta_openapi_fragment() -- never a hand-copied fragment
// literal.
//
// SCOPE (narrow, per B1-1181b design): this test registers exactly two real
// producers (bb_mqtt_client_health_register(), bb_temp_register_info()) in a
// TEST-CHOSEN fixed order, then calls the RAW bb_health_assemble_schema()
// directly (not the lazy-cached bb_health_get_assembled_schema() wrapper --
// avoids cross-test cache pollution). This proves the SPLICE ALGORITHM
// (base + ,"name": + section fragment, per section, + suffix), not any
// specific firmware's section set -- no per-target manifest exists, and
// this test does not generalize to one.
//
// Golden construction: each section's schema_props ("{"type":"object",
// "properties":{...}}") is reconstructed, not hand-copied, by rendering the
// section's real _desc/_meta pair through bb_serialize_meta_openapi_fragment()
// (the section-fragment engine mode; same host meta engine every other
// B1-1059 golden uses). No field name/type content is duplicated.
#if defined(BB_HEALTH_SECTION_TESTING) && defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_health_section.h"
#include "bb_mqtt_client.h"
#include "bb_temp.h"

#include "bb_serialize_meta.h"

#include "../../components/bb_health/bb_health_schema_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Reconstructs the bare section-object form of a schema_props literal
// ("{"type":"object","properties":{...}}", no required/additionalProperties)
// from a section's real desc+meta pair, via the section-fragment engine
// mode. Never hand-copies field content.
static void build_bare_section_schema(const bb_serialize_desc_t      *desc,
                                       const bb_serialize_desc_meta_t *meta,
                                       char *out, size_t out_size)
{
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_fragment(desc, meta, out, out_size, &n));
}

// ---------------------------------------------------------------------------
// The golden itself.
// ---------------------------------------------------------------------------

void test_bb_health_schema_composite_meta_golden_mqtt_then_temp(void)
{
    bb_health_section_test_reset();

    // Fixed test-chosen registration order: mqtt, then temp. Stands in for
    // "whatever a composed target's order would be" -- proves the splice
    // algorithm, not any specific firmware's section set.
    bb_mqtt_client_health_register();
    bb_temp_register_info();

    char mqtt_section[512];
    build_bare_section_schema(&bb_mqtt_client_health_section_desc,
                               &bb_mqtt_client_health_section_meta,
                               mqtt_section, sizeof mqtt_section);

    char temp_section[512];
    build_bare_section_schema(&bb_temp_health_desc, &bb_temp_health_meta,
                               temp_section, sizeof temp_section);

    char expected[2048];
    int written = snprintf(expected, sizeof expected, "%s,\"mqtt\":%s,\"temp\":%s%s",
                            k_health_base, mqtt_section, temp_section, k_health_suffix);
    TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof expected);

    char *actual = bb_health_assemble_schema();
    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT_EQUAL_STRING(expected, actual);
    free(actual);

    bb_health_section_test_reset();
}

// Injection-verify (per B1-1181b instructions): swapping section order in
// the hand-golden must fail against the real (mqtt-then-temp) composer
// output -- proves the byte-equal assert above is actually order-sensitive,
// not vacuously true. See the PR report for the captured RED/GREEN
// transcript of this exercise (temporarily swapping the order below to
// mqtt-after-temp reproduces a RED failure; restoring it turns GREEN again).
void test_bb_health_schema_composite_meta_golden_wrong_order_fails_to_match(void)
{
    bb_health_section_test_reset();

    bb_mqtt_client_health_register();
    bb_temp_register_info();

    char mqtt_section[512];
    build_bare_section_schema(&bb_mqtt_client_health_section_desc,
                               &bb_mqtt_client_health_section_meta,
                               mqtt_section, sizeof mqtt_section);

    char temp_section[512];
    build_bare_section_schema(&bb_temp_health_desc, &bb_temp_health_meta,
                               temp_section, sizeof temp_section);

    // Deliberately wrong order: temp before mqtt -- the real registration
    // order above is mqtt-then-temp, so this must NOT match.
    char wrong_order[2048];
    int written = snprintf(wrong_order, sizeof wrong_order, "%s,\"temp\":%s,\"mqtt\":%s%s",
                            k_health_base, temp_section, mqtt_section, k_health_suffix);
    TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof wrong_order);

    char *actual = bb_health_assemble_schema();
    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT_TRUE(strcmp(wrong_order, actual) != 0);
    free(actual);

    bb_health_section_test_reset();
}

#endif /* BB_HEALTH_SECTION_TESTING && BB_SERIALIZE_META_HOST */
