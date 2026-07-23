// test_bb_diag_tasks_get_wire_meta_golden -- B1-1191 diag conversion:
// proves the #if-gated co-located bb_serialize_desc_meta_t pattern
// (B1-1059 PR-2a exemplar, test_bb_wifi_http_wire_meta_golden.c) for GET
// /api/diag/tasks (bb_diag_tasks_get_wire_desc / bb_diag_tasks_get_wire_meta,
// both in components/bb_diag_http/bb_diag_tasks_get_wire.c). Only reachable
// when BB_SERIALIZE_META_HOST is defined (this native env; see
// platformio.ini) -- the ESP-IDF/device build never defines it, so
// bb_diag_tasks_get_wire_meta doesn't exist there.
//
// Fidelity finding vs the UNCHANGED hand-authored s_tasks_get_responses[]
// 200 literal (platform/espidf/bb_diag_http/bb_diag_http_routes.c): the
// descriptor's present-gating (core/runtime/stack_budget_bytes/
// wdt_subscribed/sw_wdt_* all optional; name/prio/base_prio/stack_hwm/state
// always required; registry.{count,capacity,dropped} always required)
// matches the hand literal's own field list content BYTE-IDENTICALLY.
// Three structural differences are inherent to the meta engine's fixed
// schema-shape policy, not this table's content, and are accepted as
// documented deltas (not a fidelity failure), same precedent as
// test_bb_ota_validator_partitions_wire_meta_golden.c and
// test_bb_diag_sockets_get_wire_meta_golden.c:
//   1. a trailing "additionalProperties":false at every object depth (top
//      level, "registry", and the "tasks" items object) -- the engine
//      always closes every rendered object; the hand literal predates that
//      policy and never set it anywhere.
//   2. the "tasks" ARR-of-OBJ items object never gets a "required" list --
//      bb_serialize_meta_openapi_schema()'s bb_oa_write_items() only emits
//      "type"/"properties"/"additionalProperties" for an object-shaped
//      array element, unlike bb_oa_write_field_schema()'s direct-
//      BB_TYPE_OBJ branch (which DOES emit "required" -- see "registry"
//      below, which IS a direct BB_TYPE_OBJ field and DOES carry its
//      "required" list). This engine limitation applies regardless of the
//      array's cardinality (BB_ARR_FIXED vs this table's BB_ARR_STREAM) --
//      the meta engine never inspects `.cardinality` at all, it only reads
//      `.children`/`.elem_type`.
//   3. the hand literal's top-level "required" list is `["tasks",
//      "registry"]` (both roots unconditional) -- reproduced identically
//      here since neither top-level field carries a `.present` predicate.
#if defined(BB_SERIALIZE_META_HOST)

#include "unity.h"

#include "bb_serialize_meta.h"

#include "../../components/bb_diag_http/bb_diag_tasks_get_wire_priv.h"

#include <string.h>

// Byte-fidelity target: the UNCHANGED s_tasks_get_responses[]'s
// "properties"/"required" content, re-expressed as
// bb_serialize_meta_openapi_schema()'s fixed object-schema shape (see file
// banner for the three documented deltas).
static const char *const k_expected_meta_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"tasks\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\"},"
    "\"prio\":{\"type\":\"integer\"},"
    "\"base_prio\":{\"type\":\"integer\"},"
    "\"stack_hwm\":{\"type\":\"integer\"},"
    "\"state\":{\"type\":\"string\"},"
    "\"core\":{\"type\":\"integer\"},"
    "\"runtime\":{\"type\":\"integer\"},"
    "\"stack_budget_bytes\":{\"type\":\"integer\"},"
    "\"wdt_subscribed\":{\"type\":\"boolean\"},"
    "\"sw_wdt_timeout_ms\":{\"type\":\"integer\"},"
    "\"sw_wdt_last_feed_age_ms\":{\"type\":\"integer\"},"
    "\"sw_wdt_miss_count\":{\"type\":\"integer\"},"
    "\"sw_wdt_last_miss_age_ms\":{\"type\":\"integer\"}},"
    "\"additionalProperties\":false}},"
    "\"registry\":{\"type\":\"object\",\"properties\":{"
    "\"count\":{\"type\":\"integer\"},"
    "\"capacity\":{\"type\":\"integer\"},"
    "\"dropped\":{\"type\":\"integer\"}},"
    "\"required\":[\"count\",\"capacity\",\"dropped\"],"
    "\"additionalProperties\":false}},"
    "\"required\":[\"tasks\",\"registry\"],"
    "\"additionalProperties\":false}";

// The co-located table must first structurally agree with its paired
// descriptor (type_name match, one row per field, no orphans, recursing
// into both "tasks"'s row children and "registry"'s children) -- this is
// the same gate a future host generator would run before ever trusting the
// table enough to render a schema from it.
void test_bb_diag_tasks_get_wire_meta_validates_against_desc(void)
{
    char err[128] = { 0 };
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_validate(&bb_diag_tasks_get_wire_desc,
                                    &bb_diag_tasks_get_wire_meta,
                                    err, sizeof err));
}

// The golden itself: bb_serialize_meta_openapi_schema() over the real
// production desc + the real production co-located meta table (not a
// test-local copy of either) reproduces the UNCHANGED s_tasks_get_responses[]
// field-level content exactly, modulo the three documented structural
// deltas above.
void test_bb_diag_tasks_get_wire_meta_golden_matches_hand_literal(void)
{
    char   buf[2048];
    size_t n = 0;

    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_schema(&bb_diag_tasks_get_wire_desc,
                                          &bb_diag_tasks_get_wire_meta,
                                          buf, sizeof buf, &n));

    TEST_ASSERT_EQUAL_STRING(k_expected_meta_schema, buf);
    TEST_ASSERT_EQUAL_UINT(strlen(k_expected_meta_schema), n);
}

#endif /* BB_SERIALIZE_META_HOST */
