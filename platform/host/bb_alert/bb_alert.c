// bb_alert — platform-shared implementation (host + espidf).
// Pure serializer + emit, no platform-specific types.

#include "bb_alert.h"
#include "bb_openapi.h"

#if BB_ALERT_ENABLE

#include "bb_event.h"
#include "bb_json.h"
#include "bb_clock.h"
#include "bb_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "bb_alert";
static bb_event_topic_t s_topic = NULL;

#ifdef BB_ALERT_TESTING
static bb_alert_severity_t s_min_sev = (bb_alert_severity_t)BB_ALERT_MIN_SEVERITY;
#define EFFECTIVE_MIN_SEV s_min_sev
#else
#define EFFECTIVE_MIN_SEV ((bb_alert_severity_t)BB_ALERT_MIN_SEVERITY)
#endif

// Pure serializer — no ESP-IDF types.
static char *alert_serialize(const char *type, bb_alert_severity_t sev,
                             bb_alert_fill_fn fill, void *ctx)
{
    bb_json_t obj = bb_json_obj_new();
    if (!obj) return NULL;
    bb_json_obj_set_string(obj, "type", type);
    bb_json_obj_set_int(obj, "severity", (int64_t)sev);
    bb_json_obj_set_int(obj, "uptime_ms", (int64_t)bb_clock_now_ms64());
    if (fill) fill(obj, ctx);
    char *str = bb_json_serialize(obj);
    bb_json_free(obj);
    return str;
}

void bb_alert_emit(const char *type, bb_alert_severity_t sev,
                   bb_alert_fill_fn fill, void *ctx)
{
    if (sev < EFFECTIVE_MIN_SEV) return;
    if (!s_topic) return;
    char *payload = alert_serialize(type, sev, fill, ctx);
    if (!payload) {
        bb_log_w(TAG, "alert_serialize alloc failed");
        return;
    }
    bb_event_post(s_topic, 0, payload, strlen(payload) + 1);
    bb_json_free_str(payload);
}

static const char k_alert_schema[] =
    "{\"title\":\"Alert\",\"x-sse-topic\":\"alert\",\"type\":\"object\","
    "\"properties\":{"
    "\"type\":{\"type\":\"string\"},"
    "\"severity\":{\"type\":\"integer\"},"
    "\"uptime_ms\":{\"type\":\"integer\"}},"
    "\"required\":[\"type\",\"severity\",\"uptime_ms\"]}";

bb_err_t bb_alert_register(void)
{
    bb_openapi_register_topic_schema("alert", k_alert_schema, "Alert");
    return bb_event_topic_register("alert", &s_topic);
}

#ifdef BB_ALERT_TESTING
void bb_alert_reset_for_test(void)
{
    s_topic = NULL;
    s_min_sev = (bb_alert_severity_t)BB_ALERT_MIN_SEVERITY;
}

bb_event_topic_t bb_alert_topic_for_test(void)
{
    return s_topic;
}

void bb_alert_set_min_severity_for_test(bb_alert_severity_t sev)
{
    s_min_sev = sev;
}
#endif

#endif // BB_ALERT_ENABLE
