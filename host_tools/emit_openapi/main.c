#include "bb_openapi.h"
#include "bb_json.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Test fixtures (gated on BB_OPENAPI_HARNESS_FIXTURES)
// Consumer projects link their own describe-routes TUs instead and omit this
// flag, keeping the same main() entrypoint.
// ---------------------------------------------------------------------------

#ifdef BB_OPENAPI_HARNESS_FIXTURES

#include "bb_http.h"
#include "bb_http_server.h"

static bb_err_t stub_handler(bb_http_request_t *req)
{
    (void)req;
    return BB_OK;
}

static const bb_route_response_t s_stats_responses[] = {
    {
        .status       = 200,
        .content_type = "application/json",
        .schema       = "{\"type\":\"object\",\"properties\":{\"hashrate\":{\"type\":\"number\"}}}",
        .description  = "mining stats",
    },
    { .status = 0 },
};

static const bb_route_t s_route_stats = {
    .method               = BB_HTTP_GET,
    .path                 = "/api/stats",
    .tag                  = "mining",
    .summary              = "Mining and device telemetry",
    .operation_id         = "getStats",
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_stats_responses,
    .handler              = stub_handler,
};

static const bb_route_response_t s_pool_responses[] = {
    {
        .status       = 204,
        .content_type = NULL,
        .schema       = NULL,
        .description  = "accepted",
    },
    { .status = 0 },
};

static const bb_route_t s_route_pool = {
    .method               = BB_HTTP_POST,
    .path                 = "/api/pool-config",
    .tag                  = "config",
    .summary              = "Update pool configuration",
    .operation_id         = NULL,
    .request_content_type = "application/json",
    .request_schema       = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}}}",
    .responses            = s_pool_responses,
    .handler              = stub_handler,
};

static const bb_route_response_t s_health_responses[] = {
    {
        .status       = 200,
        .content_type = "text/plain",
        .schema       = NULL,
        .description  = "ok",
    },
    { .status = 0 },
};

static const bb_route_t s_route_health = {
    .method               = BB_HTTP_GET,
    .path                 = "/api/health",
    .tag                  = NULL,
    .summary              = "Health check",
    .operation_id         = NULL,
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_health_responses,
    .handler              = stub_handler,
};

// SSE fixture — text/event-stream + schema==NULL drives the oneOf synthesis
// against SSE-tagged registry entries (LogEvent). Mirrors bb_event_routes'
// GET /api/events so the emitted doc exercises the full $ref/oneOf pipeline.
static const bb_route_response_t s_events_responses[] = {
    {
        .status       = 200,
        .content_type = "text/event-stream",
        .schema       = NULL,
        .description  = "Server-Sent Events stream",
    },
    { .status = 0 },
};

static const bb_route_t s_route_events = {
    .method               = BB_HTTP_GET,
    .path                 = "/api/events",
    .tag                  = "events",
    .summary              = "Subscribe to the Server-Sent Events stream",
    .operation_id         = NULL,
    .request_content_type = NULL,
    .request_schema       = NULL,
    .responses            = s_events_responses,
    .handler              = stub_handler,
};

// Schema literals duplicated minimally from the platform files (not linked into
// this host tool): keep byte-identical to k_log_event_schema (bb_log_event.c)
// and k_wifi_info_schema (bb_wifi_routes.c).
static const char k_log_event_schema[] =
    "{\"title\":\"LogEvent\",\"x-sse-topic\":\"log\",\"type\":\"object\","
    "\"properties\":{"
    "\"ts\":{\"type\":\"integer\"},"
    "\"level\":{\"type\":\"string\",\"enum\":[\"I\",\"W\",\"E\",\"D\",\"V\",\"?\"]},"
    "\"tag\":{\"type\":\"string\"},"
    "\"msg\":{\"type\":\"string\"}},"
    "\"required\":[\"ts\",\"level\",\"tag\",\"msg\"]}";

static const char k_wifi_info_schema[] =
    "{\"title\":\"WifiInfo\",\"type\":\"object\","
    "\"properties\":{"
    "\"ssid\":{\"type\":\"string\"},"
    "\"bssid\":{\"type\":\"string\"},"
    "\"rssi\":{\"type\":\"integer\"},"
    "\"ip\":{\"type\":\"string\"},"
    "\"connected\":{\"type\":\"boolean\"},"
    "\"disc_reason\":{\"type\":\"string\"},"
    "\"disc_age_s\":{\"type\":\"integer\"},"
    "\"retry_count\":{\"type\":\"integer\"}},"
    "\"required\":[\"ssid\",\"connected\"]}";

static void register_fixtures(void)
{
    bb_http_route_registry_clear();
    bb_http_register_described_route(NULL, &s_route_stats);
    bb_http_register_described_route(NULL, &s_route_pool);
    bb_http_register_described_route(NULL, &s_route_health);
    bb_http_register_described_route(NULL, &s_route_events);

    bb_openapi_schema_registry_clear();
    bb_openapi_register_topic_schema("log", k_log_event_schema, "LogEvent");
    bb_openapi_register_schema("WifiInfo", k_wifi_info_schema, NULL);
}

#endif /* BB_OPENAPI_HARNESS_FIXTURES */

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    const char *title       = "breadboard API";
    const char *version     = "0.0.0";
    const char *description = NULL;
    const char *server_url  = NULL;

    // Simple positional args: title version [description [server_url]]
    if (argc > 1) title       = argv[1];
    if (argc > 2) version     = argv[2];
    if (argc > 3) description = argv[3];
    if (argc > 4) server_url  = argv[4];

#ifdef BB_OPENAPI_HARNESS_FIXTURES
    register_fixtures();
#endif

    bb_openapi_meta_t meta = {
        .title       = title,
        .version     = version,
        .description = description,
        .server_url  = server_url,
    };

    bb_json_t doc = bb_openapi_emit(&meta);
    if (!doc) {
        fprintf(stderr, "bb_openapi_emit failed\n");
        return 1;
    }

    char *json = bb_json_serialize(doc);
    bb_json_free(doc);

    if (!json) {
        fprintf(stderr, "bb_json_serialize failed\n");
        return 1;
    }

    fputs(json, stdout);
    fputc('\n', stdout);
    bb_json_free_str(json);

    return 0;
}
