// test_bb_diag_sections_openapi_described -- B1-1180 PR-1: proves that the
// six /api/diag/<name> sections' hand-authored schemas (bb_meminfo_heap_snap_schema,
// bb_diag_storage_nvs_schema, bb_diag_storage_partitions_schema,
// bb_ring_diag_schema, bb_wifi_http_diag_schema, bb_ws_server_diag_schema --
// each portable/on-device, defined alongside its owning bb_serialize_desc_t)
// make their GET routes VISIBLE to bb_openapi_emit() once described via
// bb_http_register_route_descriptor_only() (handler=NULL, describe-only --
// the SAME mechanism platform/espidf/bb_diag_http/
// bb_diag_http_section_dispatch.c's diag_sections_describe() drives at
// runtime for every registered section carrying a non-NULL `describe_route`,
// see that file). This test can't drive diag_sections_describe() itself
// (it's ESP-IDF-only glue over bb_http_request_t, unreachable from a host
// build) -- instead it builds the SAME six bb_route_t descriptors by hand,
// referencing the real production schema constants (not test-local copies),
// and proves bb_openapi_emit() surfaces all six paths -- the one thing
// diag_sections_describe() itself guarantees for any section whose
// `describe_route` is non-NULL.

#include "unity.h"

#include "bb_openapi.h"
#include "bb_http.h"
#include "bb_http_server.h"
#include "bb_json.h"

#include "bb_diag_storage_nvs.h"
#include "bb_diag_storage_partitions.h"
#include "bb_meminfo_heap_snap.h"
#include "bb_ring_diag.h"
#include "bb_wifi_http_diag.h"
#include "bb_ws_server_diag.h"

#include <stddef.h>

// ---------------------------------------------------------------------------
// One descriptor per section, mirroring diag_sections_describe()'s own
// route shape (GET, tag="diag", handler=NULL, single 200 application/json
// response carrying the section's real schema). The response tables are
// filled at test-fixture setup (register_all_diag_section_routes(), below)
// rather than statically initialized, since the real production schema
// constants (bb_meminfo_heap_snap_schema, etc.) are themselves `const char
// *const` OBJECTS whose address bb_http_register_route_descriptor_only()
// only needs at runtime -- same as diag_sections_describe() itself, which
// builds its route tables at runtime for the exact same reason (see
// platform/espidf/bb_diag_http/bb_diag_http_section_dispatch.c).
// ---------------------------------------------------------------------------

static bb_route_response_t s_meminfo_responses_live[2];
static bb_route_response_t s_storage_nvs_responses_live[2];
static bb_route_response_t s_storage_partitions_responses_live[2];
static bb_route_response_t s_rings_responses_live[2];
static bb_route_response_t s_wifi_responses_live[2];
static bb_route_response_t s_websocket_responses_live[2];

static bb_route_t s_route_meminfo = {
    .method = BB_HTTP_GET, .path = "/api/diag/meminfo", .tag = "diag",
    .summary = "heap memory snapshot", .responses = s_meminfo_responses_live, .handler = NULL,
};
static bb_route_t s_route_storage_nvs = {
    .method = BB_HTTP_GET, .path = "/api/diag/storage/nvs", .tag = "diag",
    .summary = "NVS storage inventory", .responses = s_storage_nvs_responses_live, .handler = NULL,
};
static bb_route_t s_route_storage_partitions = {
    .method = BB_HTTP_GET, .path = "/api/diag/storage/partitions", .tag = "diag",
    .summary = "Partition table inventory", .responses = s_storage_partitions_responses_live, .handler = NULL,
};
static bb_route_t s_route_rings = {
    .method = BB_HTTP_GET, .path = "/api/diag/rings", .tag = "diag",
    .summary = "every live bb_queue_t", .responses = s_rings_responses_live, .handler = NULL,
};
static bb_route_t s_route_wifi = {
    .method = BB_HTTP_GET, .path = "/api/diag/wifi", .tag = "diag",
    .summary = "WiFi diagnostic surface", .responses = s_wifi_responses_live, .handler = NULL,
};
static bb_route_t s_route_websocket = {
    .method = BB_HTTP_GET, .path = "/api/diag/websocket", .tag = "diag",
    .summary = "open WebSocket connections", .responses = s_websocket_responses_live, .handler = NULL,
};

static void register_all_diag_section_routes(void)
{
    s_meminfo_responses_live[0]             = (bb_route_response_t){ .status = 200, .content_type = "application/json", .schema = bb_meminfo_heap_snap_schema };
    s_storage_nvs_responses_live[0]         = (bb_route_response_t){ .status = 200, .content_type = "application/json", .schema = bb_diag_storage_nvs_schema };
    s_storage_partitions_responses_live[0]  = (bb_route_response_t){ .status = 200, .content_type = "application/json", .schema = bb_diag_storage_partitions_schema };
    s_rings_responses_live[0]               = (bb_route_response_t){ .status = 200, .content_type = "application/json", .schema = bb_ring_diag_schema };
    s_wifi_responses_live[0]                = (bb_route_response_t){ .status = 200, .content_type = "application/json", .schema = bb_wifi_http_diag_schema };
    s_websocket_responses_live[0]           = (bb_route_response_t){ .status = 200, .content_type = "application/json", .schema = bb_ws_server_diag_schema };

    bb_http_route_registry_clear();
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route_descriptor_only(&s_route_meminfo));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route_descriptor_only(&s_route_storage_nvs));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route_descriptor_only(&s_route_storage_partitions));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route_descriptor_only(&s_route_rings));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route_descriptor_only(&s_route_wifi));
    TEST_ASSERT_EQUAL(BB_OK, bb_http_register_route_descriptor_only(&s_route_websocket));
}

// The core assertion: every one of the six describe-only routes appears in
// bb_openapi_emit()'s "paths", each carrying its section's real
// bb_diag_section_t.describe_route's response schema as an injected JSON
// object (never a string) -- bb_http_register_route_descriptor_only()
// (handler=NULL) never touches
// bb_http_server's live exact-match s_dispatch[] table (see
// route_registry.c's doc comment), so this only exercises the OpenAPI
// registry side, never the actual GET /api/diag/* wildcard dispatch.
void test_bb_diag_sections_openapi_described_paths_present(void)
{
    register_all_diag_section_routes();

    bb_openapi_meta_t meta = { .title = "Test", .version = "1.0.0" };
    bb_json_t doc = bb_openapi_emit(&meta);
    TEST_ASSERT_NOT_NULL(doc);

    bb_json_t paths = bb_json_obj_get_item(doc, "paths");
    TEST_ASSERT_NOT_NULL(paths);

    static const char *const k_paths[] = {
        "/api/diag/meminfo",
        "/api/diag/storage/nvs",
        "/api/diag/storage/partitions",
        "/api/diag/rings",
        "/api/diag/wifi",
        "/api/diag/websocket",
    };

    for (size_t i = 0; i < sizeof(k_paths) / sizeof(k_paths[0]); i++) {
        bb_json_t path_item = bb_json_obj_get_item(paths, k_paths[i]);
        TEST_ASSERT_NOT_NULL(path_item);

        bb_json_t get_op = bb_json_obj_get_item(path_item, "get");
        TEST_ASSERT_NOT_NULL(get_op);

        bb_json_t resps   = bb_json_obj_get_item(get_op, "responses");
        bb_json_t r200    = bb_json_obj_get_item(resps, "200");
        bb_json_t content = bb_json_obj_get_item(r200, "content");
        bb_json_t media   = bb_json_obj_get_item(content, "application/json");
        bb_json_t schema  = bb_json_obj_get_item(media, "schema");

        TEST_ASSERT_NOT_NULL(schema);
        TEST_ASSERT_TRUE(bb_json_item_is_object(schema));
    }

    bb_json_free(doc);
}
