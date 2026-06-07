#include "unity.h"
#include "bb_http_extender.h"
#include "bb_http_extender_test.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Extender callbacks
// ---------------------------------------------------------------------------

static int s_call_count_a = 0;
static int s_call_count_b = 0;

static void extender_a(void *root)
{
    (void)root;
    s_call_count_a++;
}

static void extender_b(void *root)
{
    (void)root;
    s_call_count_b++;
}

// Ordered-call tracking for insertion-order test
static int s_order[4];
static int s_order_idx = 0;

static void fn_order_a(void *r)
{
    (void)r;
    s_order[s_order_idx++] = 1;
}

static void fn_order_b(void *r)
{
    (void)r;
    s_order[s_order_idx++] = 2;
}

static void local_reset(void)
{
    s_call_count_a = 0;
    s_call_count_b = 0;
    s_order_idx    = 0;
}

// ---------------------------------------------------------------------------
// Registration tests
// ---------------------------------------------------------------------------

// NULL fn → BB_ERR_INVALID_ARG
void test_bb_http_extender_null_fn_returns_invalid_arg(void)
{
    local_reset();
    bb_err_t err = bb_http_register_route_extender("info", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_ARG, err);
}

// Valid registration returns BB_OK
void test_bb_http_extender_register_returns_ok(void)
{
    local_reset();
    bb_err_t err = bb_http_register_route_extender("info", extender_a, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);
}

// Register after freeze → BB_ERR_INVALID_STATE
void test_bb_http_extender_register_after_freeze_returns_invalid_state(void)
{
    local_reset();
    bb_http_extender_freeze();
    bb_err_t err = bb_http_register_route_extender("info", extender_a, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_INVALID_STATE, err);
}

// Per-route capacity: BB_HTTP_EXTENDER_MAX_PER_ROUTE fit; one more → NO_SPACE
void test_bb_http_extender_per_route_capacity(void)
{
    local_reset();
    for (int i = 0; i < BB_HTTP_EXTENDER_MAX_PER_ROUTE; i++) {
        bb_err_t err = bb_http_register_route_extender("cap_route", extender_a, NULL);
        TEST_ASSERT_EQUAL_INT(BB_OK, err);
    }
    bb_err_t err = bb_http_register_route_extender("cap_route", extender_a, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
}

// Two distinct route_ids don't interfere with each other's capacity.
void test_bb_http_extender_two_route_ids_independent(void)
{
    local_reset();
    bb_err_t err_a = bb_http_register_route_extender("info",   extender_a, NULL);
    bb_err_t err_b = bb_http_register_route_extender("health", extender_b, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, err_a);
    TEST_ASSERT_EQUAL_INT(BB_OK, err_b);
}

// ---------------------------------------------------------------------------
// run_extenders tests
// ---------------------------------------------------------------------------

// run_extenders for a route with no extenders is a no-op (no crash).
void test_bb_http_extender_run_no_extenders_noop(void)
{
    local_reset();
    bb_http_route_run_extenders("empty_route", NULL);
    TEST_ASSERT_EQUAL_INT(0, s_call_count_a);
}

// run_extenders invokes the right callbacks in registration order.
void test_bb_http_extender_run_calls_in_order(void)
{
    local_reset();
    bb_http_register_route_extender("order_route", fn_order_a, NULL);
    bb_http_register_route_extender("order_route", fn_order_b, NULL);

    bb_http_route_run_extenders("order_route", NULL);

    TEST_ASSERT_EQUAL_INT(2, s_order_idx);
    TEST_ASSERT_EQUAL_INT(1, s_order[0]);
    TEST_ASSERT_EQUAL_INT(2, s_order[1]);
}

// run_extenders for route A does NOT call route B's extenders.
void test_bb_http_extender_run_isolates_routes(void)
{
    local_reset();
    bb_http_register_route_extender("info",   extender_a, NULL);
    bb_http_register_route_extender("health", extender_b, NULL);

    bb_http_route_run_extenders("info", NULL);
    TEST_ASSERT_EQUAL_INT(1, s_call_count_a);
    TEST_ASSERT_EQUAL_INT(0, s_call_count_b);

    bb_http_route_run_extenders("health", NULL);
    TEST_ASSERT_EQUAL_INT(1, s_call_count_a);
    TEST_ASSERT_EQUAL_INT(1, s_call_count_b);
}

// ---------------------------------------------------------------------------
// assemble_schema tests
// ---------------------------------------------------------------------------

// No extenders: assembled == base + suffix
void test_bb_http_extender_assemble_no_extenders_base_plus_suffix(void)
{
    local_reset();
    static const char base[]   = "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"string\"}";
    static const char suffix[] = "},\"required\":[\"x\"]}";

    const char *schema = bb_http_route_assemble_schema("no_ext_route", base, suffix);
    TEST_ASSERT_NOT_NULL(schema);

    char expected[512];
    snprintf(expected, sizeof(expected), "%s%s", base, suffix);
    TEST_ASSERT_EQUAL_STRING(expected, schema);

    // Must be valid JSON
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "assembled schema (no extenders) is not valid JSON");
    cJSON_Delete(parsed);
}

// NULL/empty fragment: treated as no contribution
void test_bb_http_extender_assemble_null_fragment_skipped(void)
{
    local_reset();
    bb_http_register_route_extender("null_frag_route", extender_a, NULL);
    bb_http_register_route_extender("null_frag_route", extender_b, "");

    static const char base[]   = "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"string\"}";
    static const char suffix[] = "}}";
    const char *schema = bb_http_route_assemble_schema("null_frag_route", base, suffix);
    TEST_ASSERT_NOT_NULL(schema);

    // Should equal base+suffix (no fragments contributed)
    char expected[256];
    snprintf(expected, sizeof(expected), "%s%s", base, suffix);
    TEST_ASSERT_EQUAL_STRING(expected, schema);
}

// One fragment: appears in assembled schema
void test_bb_http_extender_assemble_one_fragment(void)
{
    local_reset();
    static const char frag[] = "\"xtest\":{\"type\":\"string\"}";
    bb_http_register_route_extender("one_frag_route", extender_a, frag);

    static const char base[]   = "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"integer\"}";
    static const char suffix[] = "}}";
    const char *schema = bb_http_route_assemble_schema("one_frag_route", base, suffix);
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, frag), "fragment not in assembled schema");

    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "assembled schema with one fragment is not valid JSON");
    cJSON_Delete(parsed);
}

// Two fragments: both appear in assembled schema, valid JSON
void test_bb_http_extender_assemble_two_fragments(void)
{
    local_reset();
    static const char frag1[] = "\"aa\":{\"type\":\"string\"}";
    static const char frag2[] = "\"bb\":{\"type\":\"integer\"}";
    bb_http_register_route_extender("two_frag_route", extender_a, frag1);
    bb_http_register_route_extender("two_frag_route", extender_b, frag2);

    static const char base[]   = "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"boolean\"}";
    static const char suffix[] = "}}";
    const char *schema = bb_http_route_assemble_schema("two_frag_route", base, suffix);
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, frag1), "frag1 not in assembled schema");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(schema, frag2), "frag2 not in assembled schema");

    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "assembled schema with two fragments is not valid JSON");
    cJSON_Delete(parsed);
}

// Assembled schema pointer is stable (get_assembled returns same pointer).
void test_bb_http_extender_get_assembled_schema_cached(void)
{
    local_reset();
    static const char base[]   = "{\"type\":\"object\",\"properties\":{";
    static const char suffix[] = "}}";
    const char *first  = bb_http_route_assemble_schema("cache_route", base, suffix);
    const char *second = bb_http_extender_get_assembled_schema("cache_route");
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_EQUAL_PTR(first, second);
}

// ---------------------------------------------------------------------------
// reset clears all state
// ---------------------------------------------------------------------------

void test_bb_http_extender_reset_clears_state(void)
{
    local_reset();
    bb_http_register_route_extender("info", extender_a, "\"x\":{\"type\":\"string\"}");
    bb_http_extender_freeze();

    // After reset: frozen clears, extenders gone
    bb_http_extender_reset_for_test();

    // Can register again (not frozen)
    bb_err_t err = bb_http_register_route_extender("info", extender_a, NULL);
    TEST_ASSERT_EQUAL_INT(BB_OK, err);

    // No assembled schema stored yet
    const char *schema = bb_http_extender_get_assembled_schema("info");
    TEST_ASSERT_NULL_MESSAGE(schema, "assembled schema should be NULL after reset before assemble");
}

// ---------------------------------------------------------------------------
// Route table full test
// ---------------------------------------------------------------------------

// Register one extender under each of BB_HTTP_EXTENDER_MAX_ROUTES distinct
// route_ids; verify all succeed. Then try one more and expect NO_SPACE.
void test_bb_http_extender_route_table_full_returns_no_space(void)
{
    local_reset();

    // Static route names
    static const char *routes[] = {
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7"
    };

    // Register one extender per route (fills the table)
    for (int i = 0; i < BB_HTTP_EXTENDER_MAX_ROUTES; i++) {
        bb_err_t err = bb_http_register_route_extender(routes[i], extender_a, NULL);
        TEST_ASSERT_EQUAL_INT(BB_OK, err);
    }

    // Try to register under one more distinct route_id — should fail
    bb_err_t err = bb_http_register_route_extender("r_overflow", extender_a, NULL);
    TEST_ASSERT_EQUAL_INT(BB_ERR_NO_SPACE, err);
}

// ---------------------------------------------------------------------------
// Assemble schema for route with no registered extenders
// ---------------------------------------------------------------------------

// Call bb_http_route_assemble_schema() for a route_id that has NO registered
// extenders; verify the result is base + suffix and a storage slot is created.
void test_bb_http_extender_assemble_no_extenders_route(void)
{
    local_reset();

    static const char base[]   = "{\"type\":\"object\",\"properties\":{";
    static const char suffix[] = "}}";
    static const char route_id[] = "freshroute";

    // Assemble without any prior registration for this route
    const char *schema = bb_http_route_assemble_schema(route_id, base, suffix);
    TEST_ASSERT_NOT_NULL(schema);

    // Should equal base + suffix
    char expected[256];
    snprintf(expected, sizeof(expected), "%s%s", base, suffix);
    TEST_ASSERT_EQUAL_STRING(expected, schema);

    // Verify the result is valid JSON
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "assembled schema (no extenders) should be valid JSON");
    cJSON_Delete(parsed);
}

// Assemble for a route with no extenders when table is full (false branch of line 140).
// Fill the route table with 8 registered routes, then try to assemble for a 9th route
// with no extenders. The slot cannot be created, but the buffer is still valid.
void test_bb_http_extender_assemble_no_extenders_table_full(void)
{
    local_reset();

    // Static route names
    static const char *routes[] = {
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7"
    };

    // Fill the route table with 8 extenders (one per route)
    for (int i = 0; i < BB_HTTP_EXTENDER_MAX_ROUTES; i++) {
        bb_http_register_route_extender(routes[i], extender_a, NULL);
    }

    // Now try to assemble for a route not in the table
    // This should execute the false branch on line 140
    static const char base[]   = "{\"type\":\"object\",\"properties\":{";
    static const char suffix[] = "}}";
    const char *schema = bb_http_route_assemble_schema("overflow_route", base, suffix);

    // Should still return a valid buffer (base + suffix)
    TEST_ASSERT_NOT_NULL(schema);

    char expected[256];
    snprintf(expected, sizeof(expected), "%s%s", base, suffix);
    TEST_ASSERT_EQUAL_STRING(expected, schema);

    // Verify valid JSON
    cJSON *parsed = cJSON_Parse(schema);
    TEST_ASSERT_NOT_NULL_MESSAGE(parsed, "assembled schema should be valid even when table is full");
    cJSON_Delete(parsed);
}
