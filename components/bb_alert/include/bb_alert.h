#pragma once
#include "bb_core.h"
#include "bb_json.h"
#include "bb_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BB_ALERT_INFO     = 0,
    BB_ALERT_WARNING  = 1,
    BB_ALERT_CRITICAL = 2,
} bb_alert_severity_t;

// Kconfig bridge — pattern from bb_clock.h / CLAUDE.md
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_ALERT_ENABLE
#define BB_ALERT_ENABLE CONFIG_BB_ALERT_ENABLE
#endif
#ifdef CONFIG_BB_ALERT_MIN_SEVERITY
#define BB_ALERT_MIN_SEVERITY CONFIG_BB_ALERT_MIN_SEVERITY
#endif
#endif
#ifndef BB_ALERT_ENABLE
#define BB_ALERT_ENABLE 0
#endif
#ifndef BB_ALERT_MIN_SEVERITY
#define BB_ALERT_MIN_SEVERITY 0
#endif

// Fill callback — consumer adds extra fields to the alert JSON object.
// Called synchronously from bb_alert_emit while holding no locks.
typedef void (*bb_alert_fill_fn)(bb_json_t obj, void *ctx);

// SSOT for alert "type" string literals (B1-447). Values MUST NOT change —
// they appear in the "alert" bb_event/SSE topic JSON and external clients
// (TM, dashboards) match on them.
#define BB_ALERT_TYPE_WIFI_NO_IP       "wifi_no_ip"
#define BB_ALERT_TYPE_WIFI_LOST_IP     "wifi_lost_ip"

#define BB_ALERT_TYPE_UPDATE_FAILURE   "update_failure"
#define BB_ALERT_TYPE_UPDATE_AVAILABLE "update_available"
#define BB_ALERT_TYPE_UPDATE_APPLIED   "update_applied"

#if BB_ALERT_ENABLE
void bb_alert_emit(const char *type, bb_alert_severity_t sev,
                   bb_alert_fill_fn fill, void *ctx);
bb_err_t bb_alert_register(void);

// Registers the alert topic and attaches it to /api/events.
// bbtool:init tier=regular fn=bb_alert_init server=true
bb_err_t bb_alert_init(bb_http_handle_t server);
#else
static inline void bb_alert_emit(const char *type, bb_alert_severity_t sev,
                                  bb_alert_fill_fn fill, void *ctx)
{
    (void)type; (void)sev; (void)fill; (void)ctx;
}
static inline bb_err_t bb_alert_register(void) { return 0; }
static inline bb_err_t bb_alert_init(bb_http_handle_t server) { (void)server; return 0; }
#endif

#ifdef BB_ALERT_TESTING
#include "bb_event.h"
void bb_alert_reset_for_test(void);
bb_event_topic_t bb_alert_topic_for_test(void);
void bb_alert_set_min_severity_for_test(bb_alert_severity_t sev);
#endif

#ifdef __cplusplus
}
#endif
