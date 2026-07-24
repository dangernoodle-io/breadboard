// test_bb_ota_validator_partitions_wire_schema_default -- B1-1059 emit batch
// D, site D1: config-OFF byte-identity proof for GET /api/update/partitions's
// response schema. bb_ota_validator_partitions_wire_get_schema() is a plain
// production accessor (not gated by CONFIG_BB_OPENAPI_RUNTIME_META), so this
// runs in the plain [env:native] build (config OFF, the default) and asserts
// the served schema is BB_OTA_VALIDATOR_PARTITIONS_SCHEMA_LITERAL -- the SAME
// macro text platform/espidf/bb_ota_validator/bb_ota_validator.c's config-OFF
// s_partitions_responses[0].schema uses directly (that TU is ESP-IDF-only
// and cannot link on host; this is its "live route/register schema"
// surrogate, content not pointer, KB 1492).

#include "unity.h"

#include "../../components/bb_ota_validator/bb_ota_validator_partitions_wire_priv.h"

static const char *const k_expected_ota_validator_partitions_schema =
    BB_OTA_VALIDATOR_PARTITIONS_SCHEMA_LITERAL;

void test_bb_ota_validator_partitions_wire_schema_default_matches_literal(void)
{
    TEST_ASSERT_EQUAL_STRING(k_expected_ota_validator_partitions_schema,
                              bb_ota_validator_partitions_wire_get_schema());
}
