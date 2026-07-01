// ESP-IDF route handler for bb_partition — self-registers GET /api/diag/partitions
// so the partition table (label/type/subtype/offset/size + running/next-OTA
// flags) is owned by the component that reads the partition table, mirroring
// how bb_event_routes owns /api/diag/events.
#include "bb_partition.h"
#include "bb_http.h"
#include "bb_json.h"
#include "bb_init.h"
#include "bb_log.h"

static const char *TAG = "bb_partition_routes";

// GET /api/diag/partitions — full partition table with running/next-OTA flags
static bb_err_t partitions_get_handler(bb_http_request_t *req)
{
    bb_partition_info_t parts[16];
    size_t count = 0;
    bb_partition_list(parts, sizeof(parts) / sizeof(parts[0]), &count);
    if (count > sizeof(parts) / sizeof(parts[0])) {
        count = sizeof(parts) / sizeof(parts[0]);
    }

    bb_http_json_stream_t arr;
    bb_err_t rc = bb_http_resp_json_arr_begin(req, &arr);
    if (rc != BB_OK) return rc;

    for (size_t i = 0; i < count; i++) {
        bb_json_t item = bb_json_obj_new();
        if (item) {
            bb_json_obj_set_string(item, "label",    parts[i].label);
            bb_json_obj_set_string(item, "type",     parts[i].type);
            bb_json_obj_set_string(item, "subtype",  parts[i].subtype);
            bb_json_obj_set_number(item, "offset",   (double)parts[i].offset);
            bb_json_obj_set_number(item, "size",     (double)parts[i].size);
            bb_json_obj_set_bool  (item, "running",  parts[i].running);
            bb_json_obj_set_bool  (item, "next_ota", parts[i].next_ota);
            bb_http_resp_json_arr_emit(&arr, item);
            bb_json_free(item);
        }
    }
    return bb_http_resp_json_arr_end(&arr);
}

static const bb_route_response_t s_partitions_get_responses[] = {
    { 200, "application/json",
      "{\"type\":\"array\","
      "\"items\":{\"type\":\"object\","
      "\"properties\":{"
      "\"label\":{\"type\":\"string\"},"
      "\"type\":{\"type\":\"string\"},"
      "\"subtype\":{\"type\":\"string\"},"
      "\"offset\":{\"type\":\"integer\"},"
      "\"size\":{\"type\":\"integer\"},"
      "\"running\":{\"type\":\"boolean\"},"
      "\"next_ota\":{\"type\":\"boolean\"}},"
      "\"required\":[\"label\",\"type\",\"offset\",\"size\"]}}",
      "partition table: label/type/subtype/offset/size + running and next-OTA flags" },
    { 0 },
};

static const bb_route_t s_partitions_get_route = {
    .method    = BB_HTTP_GET,
    .path      = "/api/diag/partitions",
    .tag       = "diag",
    .summary   = "Partition table: label/type/subtype/offset/size + running and next-OTA flags",
    .responses = s_partitions_get_responses,
    .handler   = partitions_get_handler,
};

static bb_err_t bb_partition_routes_init(bb_http_handle_t server)
{
    if (!server) return BB_ERR_INVALID_ARG;
    bb_err_t err = bb_http_register_described_route(server, &s_partitions_get_route);
    if (err != BB_OK) return err;
    bb_log_i(TAG, "registered /api/diag/partitions");
    return BB_OK;
}

#if CONFIG_BB_PARTITION_ROUTES_AUTOREGISTER
BB_INIT_REGISTER(bb_partition_routes, bb_partition_routes_init);
#endif
