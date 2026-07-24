#include "unity.h"
#include "bb_health_section.h"
#include "bb_mqtt_client.h"
#include "bb_temp.h"
#include "bb_serialize_meta.h"
#include "bb_serialize_meta_test.h"

#include "../../components/bb_health/bb_health_schema_priv.h"
#include "../../components/bb_health/bb_health_wire_priv.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Dedicated PlatformIO test env (native_openapi_runtime_meta, see
// platformio.ini) that builds WITH -DCONFIG_BB_OPENAPI_RUNTIME_META=1 --
// proves bb_health_assemble_schema()'s ROOT runtime-compose path (B1-1059
// PR-d) actually wires up: the "ok"/"network" root-identity slice
// (bb_health_wire_desc/_meta) is opened via
// bb_serialize_meta_openapi_open_fragment(), every registered
// bb_health_section's schema_props is spliced in (mqtt then temp, the same
// fixed test-chosen registration order as
// test_bb_health_schema_composite_meta_golden.c's config-OFF golden), then
// the root is closed via bb_serialize_meta_openapi_root_close().
//
// Calls the RAW bb_health_assemble_schema() directly (not the lazy-cached
// bb_health_get_assembled_schema() wrapper -- avoids cross-test cache
// pollution), per the config-OFF composite golden's own precedent. Golden
// construction reconstructs every piece (root open/close via the engine,
// mqtt/temp fragments via the engine's section-fragment mode) rather than
// hand-copying any field content -- same discipline as every other
// B1-1059 golden.

// Reconstructs the bare section-object form of a schema_props literal
// ("{"type":"object","properties":{...}}", no required/additionalProperties)
// from a section's real desc+meta pair, via the section-fragment engine
// mode. Never hand-copies field content.
static void build_bare_section_schema(const bb_serialize_desc_t      *desc,
                                       const bb_serialize_desc_meta_t *meta,
                                       char *out, size_t out_size)
{
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_serialize_meta_openapi_fragment(desc, meta, out, out_size, &n));
}

// Exercises the fail-fast `if (rc != BB_OK) { ...; return NULL; }` branch
// (the FIRST one, guarding the root open_fragment() call) in
// bb_health_assemble_schema()'s config-ON fork
// (platform/host/bb_health/bb_health_schema.c) -- forces the engine (via
// BB_SERIALIZE_META_TESTING's fail-injection seam, extended onto
// open_fragment()/root_close() this PR) to return BB_ERR_NO_SPACE and
// asserts the composer fails loud (NULL) rather than returning a
// partial/stale schema. Sections are registered with the seam OFF first
// (their own compose-and-patch step is independent of this test), then the
// seam is forced only around the bb_health_assemble_schema() call itself --
// unlike the mqtt/temp wiring tests' registry-level seam, this test's
// ordering does not matter relative to the matching/idempotent tests below.
void test_bb_health_assemble_schema_root_offline_on_compose_failure(void)
{
    bb_health_section_test_reset();
    bb_mqtt_client_health_register();
    bb_temp_register_info();

    bb_serialize_meta_openapi_test_set_force_no_space(true);
    char *result = bb_health_assemble_schema();
    bb_serialize_meta_openapi_test_set_force_no_space(false);

    TEST_ASSERT_NULL(result);

    bb_health_section_test_reset();
}

void test_bb_health_assemble_schema_root_composes_matching_schema(void)
{
    bb_health_section_test_reset();

    // Fixed test-chosen registration order: mqtt, then temp -- same order
    // test_bb_health_schema_composite_meta_golden.c's config-OFF golden
    // uses, standing in for "whatever a composed target's order would be".
    bb_mqtt_client_health_register();
    bb_temp_register_info();

    char   open_buf[512];
    size_t open_len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_open_fragment(&bb_health_wire_desc, &bb_health_wire_meta,
                                                 open_buf, sizeof(open_buf), &open_len));
    // open_fragment() closes root "properties" as its last byte -- strip it
    // to splice sections in as siblings of "ok"/"network" (same load-bearing
    // step bb_health_assemble_schema() itself performs).
    TEST_ASSERT_TRUE(open_len > 0 && open_buf[open_len - 1] == '}');
    open_len--;

    char mqtt_section[512];
    build_bare_section_schema(&bb_mqtt_client_health_section_desc, &bb_mqtt_client_health_section_meta,
                               mqtt_section, sizeof(mqtt_section));

    char temp_section[512];
    build_bare_section_schema(&bb_temp_health_desc, &bb_temp_health_meta,
                               temp_section, sizeof(temp_section));

    char   close_buf[128];
    size_t close_len = 0;
    TEST_ASSERT_EQUAL_INT(BB_OK,
        bb_serialize_meta_openapi_root_close(&bb_health_wire_desc, &bb_health_wire_meta,
                                              close_buf, sizeof(close_buf), &close_len));

    char expected[2048];
    int written = snprintf(expected, sizeof(expected), "%.*s,\"mqtt\":%s,\"temp\":%s}%s",
                            (int)open_len, open_buf, mqtt_section, temp_section, close_buf);
    TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof(expected));

    char *actual = bb_health_assemble_schema();
    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT_EQUAL_STRING(expected, actual);
    free(actual);

    bb_health_section_test_reset();
}

// bb_health_assemble_schema() itself has no internal cache (unlike the
// per-section ensure_*_patched() buffers) -- "idempotent" here means
// repeated calls with the same registered section set produce byte-identical
// output, each a fresh malloc() the caller must free() independently.
void test_bb_health_assemble_schema_root_idempotent_same_content(void)
{
    bb_health_section_test_reset();
    bb_mqtt_client_health_register();
    bb_temp_register_info();

    char *first = bb_health_assemble_schema();
    TEST_ASSERT_NOT_NULL(first);
    char *second = bb_health_assemble_schema();
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_EQUAL_STRING(first, second);
    free(first);
    free(second);

    bb_health_section_test_reset();
}

// Exercises the root_close()-specific fail-fast branch (bb_health_schema.c),
// distinct from the offline-on-compose-failure test above -- the global
// BB_SERIALIZE_META_TESTING seam can't isolate root_close() from the
// earlier open_fragment() call (open_fragment() always fails first, so
// root_close()'s own check is otherwise unreachable), so this uses the
// dedicated cap override (bb_health_schema_set_root_close_cap_for_test())
// instead: open_fragment() succeeds normally (real 512-byte buffer), while
// root_close() genuinely overflows a too-small cap.
void test_bb_health_assemble_schema_root_close_failure_returns_null(void)
{
    bb_health_section_test_reset();
    bb_mqtt_client_health_register();
    bb_temp_register_info();

    bb_health_schema_set_root_close_cap_for_test(4);
    char *result = bb_health_assemble_schema();
    bb_health_schema_set_root_close_cap_for_test(0);

    TEST_ASSERT_NULL(result);

    bb_health_section_test_reset();
}

static void *failing_malloc(size_t sz)
{
    (void)sz;
    return NULL;
}

// Exercises the config-ON fork's OOM branch -- mirrors
// test_bb_health.c's test_bb_health_assembled_schema_oom_returns_null()
// (the config-OFF path's own OOM test), which this env's build does NOT
// otherwise compile/run (CONFIG_BB_OPENAPI_RUNTIME_META swaps in the
// entirely separate config-ON bb_health_assemble_schema() definition).
void test_bb_health_assemble_schema_root_malloc_failure_returns_null(void)
{
    bb_health_section_test_reset();
    bb_mqtt_client_health_register();
    bb_temp_register_info();

    bb_health_schema_set_malloc(failing_malloc);
    char *result = bb_health_assemble_schema();
    bb_health_schema_set_malloc(NULL);

    TEST_ASSERT_NULL(result);

    bb_health_section_test_reset();
}

// A section registered directly (bypassing bb_mqtt_client_health_register()/
// bb_temp_register_info(), which never register with a NULL schema_props --
// on their own compose failure they just don't register at all, see
// bb_mqtt_client_health_section.c) with schema_props == NULL -- exercises
// the config-ON fork's `if (!sec->schema_props) continue;` true arm in BOTH
// its length-computation and write loops (platform/host/bb_health/
// bb_health_schema.c), proving a section left offline this way is silently
// skipped rather than crashing on a NULL schema_props dereference.
typedef struct {
    int64_t n;
} s_null_schema_snap_t;

static const bb_serialize_field_t s_null_schema_fields[] = {
    { .key = "n", .type = BB_TYPE_I64, .offset = offsetof(s_null_schema_snap_t, n) },
};

static const bb_serialize_desc_t s_null_schema_desc = {
    .type_name = "null_schema_snap_t", .fields = s_null_schema_fields, .n_fields = 1,
    .snap_size = sizeof(s_null_schema_snap_t),
};

static bb_err_t null_schema_fill(void *dst, const bb_health_fill_args_t *args)
{
    (void)args;
    ((s_null_schema_snap_t *)dst)->n = 0;
    return BB_OK;
}

void test_bb_health_assemble_schema_root_skips_section_with_null_schema_props(void)
{
    bb_health_section_test_reset();

    bb_health_section_t null_sec = {
        .name = "nullsec", .snap_desc = &s_null_schema_desc, .fill = null_schema_fill,
        .schema_props = NULL,
    };
    TEST_ASSERT_EQUAL_INT(BB_OK, bb_health_section_register(&null_sec));

    bb_mqtt_client_health_register();
    bb_temp_register_info();

    char *actual = bb_health_assemble_schema();
    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT_NULL(strstr(actual, "\"nullsec\""));
    TEST_ASSERT_NOT_NULL(strstr(actual, "\"mqtt\""));
    TEST_ASSERT_NOT_NULL(strstr(actual, "\"temp\""));
    free(actual);

    bb_health_section_test_reset();
}

// bb_health_schema_set_root_close_cap_for_test()'s override is a SHRINK-only
// knob (see its own doc comment and bb_health_schema.c's
// `if (s_test_root_close_cap && s_test_root_close_cap < root_close_cap)`
// guard) -- a cap value LARGER than the real root_close_buf must be a no-op,
// never widening the actual stack buffer. Exercises that guard's second
// condition's false arm (the close-failure test above only exercises the
// true arm, cap=4 < 128).
void test_bb_health_assemble_schema_root_close_cap_override_no_shrink_when_larger(void)
{
    bb_health_section_test_reset();
    bb_mqtt_client_health_register();
    bb_temp_register_info();

    bb_health_schema_set_root_close_cap_for_test(1000);
    char *result = bb_health_assemble_schema();
    bb_health_schema_set_root_close_cap_for_test(0);

    TEST_ASSERT_NOT_NULL(result);
    free(result);

    bb_health_section_test_reset();
}
