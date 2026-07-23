// test_bb_sensor_http_openapi_described -- B1-1180 PR-2: proves the
// /api/sensors/* hand-authored schemas (bb_sensor_http_fan_schema,
// bb_sensor_http_power_schema, bb_sensor_http_thermal_schema, each
// portable/on-device, defined alongside their owning bb_serialize_desc_t in
// components/bb_sensor_http/bb_sensor_http_wire.c) make GET+PATCH
// /api/sensors/fan, GET /api/sensors/power, and GET /api/sensors/thermal
// VISIBLE to bb_openapi_emit() once described via
// bb_http_register_route_descriptor_only() (handler=NULL, describe-only --
// the SAME mechanism bb_sensor_http_describe_routes() drives at runtime,
// see that file). This test can't drive bb_sensor_http_describe_routes()
// itself (it's ESP_PLATFORM-guarded, unreachable from a host build) --
// instead it builds the SAME four bb_route_t descriptors by hand,
// referencing the real production schema constants (not test-local
// copies), and proves bb_openapi_emit() surfaces all three paths, with
// GET+PATCH both present on /api/sensors/fan -- the one thing
// bb_sensor_http_describe_routes() itself guarantees.
//
// Mirrors test_bb_diag_sections_openapi_described.c's exact pattern
// (B1-1180 PR-1).

#include "unity.h"

#include "bb_openapi.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_json.h"

#include "../../../components/bb_sensor_http/bb_sensor_http_wire_priv.h"

#include <stddef.h>

static bb_route_response_t s_fan_get_responses_live[2];
static bb_route_response_t s_fan_patch_responses_live[2];
static bb_route_response_t s_power_get_responses_live[2];
static bb_route_response_t s_thermal_get_responses_live[2];

static bb_route_t s_route_fan_get = {
    .method = BB_HTTP_GET, .path = "/api/sensors/fan", .tag = "sensors",
    .summary = "fan configuration", .responses = s_fan_get_responses_live, .handler = NULL,
};
static bb_route_t s_route_fan_patch = {
    .method = BB_HTTP_PATCH, .path = "/api/sensors/fan", .tag = "sensors",
    .summary = "update fan configuration", .request_content_type = "application/json",
    .request_schema = NULL, .responses = s_fan_patch_responses_live, .handler = NULL,
};
static bb_route_t s_route_power_get = {
    .method = BB_HTTP_GET, .path = "/api/sensors/power", .tag = "sensors",
    .summary = "power telemetry", .responses = s_power_get_responses_live, .handler = NULL,
};
static bb_route_t s_route_thermal_get = {
    .method = BB_HTTP_GET, .path = "/api/sensors/thermal", .tag = "sensors",
    .summary = "thermal telemetry", .responses = s_thermal_get_responses_live, .handler = NULL,
};

static void register_all_sensor_routes(void)
{
    s_fan_get_responses_live[0]    = (bb_route_response_t){ .status = 200, .content_type = "application/json", .schema = bb_sensor_http_fan_schema };
    s_fan_patch_responses_live[0]  = (bb_route_response_t){ .status = 204, .content_type = NULL, .schema = NULL };
    s_power_get_responses_live[0]  = (bb_route_response_t){ .status = 200, .content_type = "application/json", .schema = bb_sensor_http_power_schema };
    s_thermal_get_responses_live[0] = (bb_route_response_t){ .status = 200, .content_type = "application/json", .schema = bb_sensor_http_thermal_schema };
    s_route_fan_patch.request_schema = bb_sensor_http_fan_request_schema;

    bb_http_route_registry_clear();
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route_descriptor_only(&s_route_fan_get));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route_descriptor_only(&s_route_fan_patch));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route_descriptor_only(&s_route_power_get));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route_descriptor_only(&s_route_thermal_get));
}

void test_bb_sensor_http_openapi_described_paths_present(void)
{
    register_all_sensor_routes();

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths = bb_json_obj_get_item(doc, "paths");
    TEST_ASSERT_NOT_NULL(paths);

    bb_json_t fan_path = bb_json_obj_get_item(paths, "/api/sensors/fan");
    TEST_ASSERT_NOT_NULL(fan_path);

    bb_json_t fan_get = bb_json_obj_get_item(fan_path, "get");
    TEST_ASSERT_NOT_NULL(fan_get);
    bb_json_t fan_get_schema = bb_json_obj_get_item(
        bb_json_obj_get_item(bb_json_obj_get_item(bb_json_obj_get_item(
            bb_json_obj_get_item(fan_get, "responses"), "200"), "content"), "application/json"), "schema");
    TEST_ASSERT_NOT_NULL(fan_get_schema);
    TEST_ASSERT_TRUE(bb_json_item_is_object(fan_get_schema));

    bb_json_t fan_patch = bb_json_obj_get_item(fan_path, "patch");
    TEST_ASSERT_NOT_NULL(fan_patch);
    bb_json_t fan_patch_req_schema = bb_json_obj_get_item(
        bb_json_obj_get_item(bb_json_obj_get_item(
            bb_json_obj_get_item(fan_patch, "requestBody"), "content"), "application/json"), "schema");
    TEST_ASSERT_NOT_NULL(fan_patch_req_schema);
    TEST_ASSERT_TRUE(bb_json_item_is_object(fan_patch_req_schema));

    bb_json_t power_path = bb_json_obj_get_item(paths, "/api/sensors/power");
    TEST_ASSERT_NOT_NULL(power_path);
    bb_json_t power_get = bb_json_obj_get_item(power_path, "get");
    TEST_ASSERT_NOT_NULL(power_get);
    // power is GET-only: this described route table never registered a
    // PATCH for /api/sensors/power (mirrors bb_sensor_http_describe_routes()
    // -- power's binding has no apply hook).
    TEST_ASSERT_NULL(bb_json_obj_get_item(power_path, "patch"));

    bb_json_t thermal_path = bb_json_obj_get_item(paths, "/api/sensors/thermal");
    TEST_ASSERT_NOT_NULL(thermal_path);
    bb_json_t thermal_get = bb_json_obj_get_item(thermal_path, "get");
    TEST_ASSERT_NOT_NULL(thermal_get);
    TEST_ASSERT_NULL(bb_json_obj_get_item(thermal_path, "patch"));

    bb_json_free(doc);
}
