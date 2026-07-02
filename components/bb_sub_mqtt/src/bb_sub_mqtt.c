#include "bb_sub_mqtt.h"
#include "bb_sub.h"
#include "bb_mqtt.h"
#include "bb_nv.h"
#include "bb_log.h"
#include "bb_init.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "bb_sub_mqtt";

#ifndef CONFIG_BB_SUB_MQTT_TOPICS
#define CONFIG_BB_SUB_MQTT_TOPICS "metrics/+/meta"
#endif

// Self-exclusion default: ON unless the build opts in to self-ingest.
#ifdef CONFIG_BB_SUB_MQTT_INGEST_SELF
#define BB_SUB_MQTT_IGNORE_SELF_DEFAULT (!(CONFIG_BB_SUB_MQTT_INGEST_SELF))
#else
#define BB_SUB_MQTT_IGNORE_SELF_DEFAULT true
#endif

static char s_topics[BB_SUB_MQTT_MAX_TOPICS][128];
static int  s_topic_count      = 0;
static bool s_kconfig_loaded   = false;
static bool s_ignore_self      = BB_SUB_MQTT_IGNORE_SELF_DEFAULT;
static bool s_shape_warned     = false;

#ifdef BB_SUB_MQTT_TESTING
static const char *s_topics_cfg_override = NULL;
#endif

// ---------------------------------------------------------------------------
// Topic filter registration
// ---------------------------------------------------------------------------

// Returns true when filter has at least 2 '/'-delimited separators, i.e. a
// 3+-segment "<prefix>/<hostname-or-wildcard>/<subtopic>" shape where a
// hostname could plausibly sit at segment index 1 (see topic_is_own_hostname
// and the self-exclusion contract in bb_sub_mqtt.h). A filter with fewer
// separators (e.g. "a/#") structurally cannot be self-excluded.
static bool filter_has_default_shape(const char *filter)
{
    const char *first = strchr(filter, '/');
    if (!first) return false;
    return strchr(first + 1, '/') != NULL;
}

bb_err_t bb_sub_mqtt_add_topic(const char *filter)
{
    if (!filter || !filter[0]) return BB_ERR_INVALID_ARG;
    if (s_topic_count >= BB_SUB_MQTT_MAX_TOPICS) {
        bb_log_w(TAG, "topic filter registry full (max %d); dropped '%s'",
                 BB_SUB_MQTT_MAX_TOPICS, filter);
        return BB_ERR_NO_SPACE;
    }
    if (s_ignore_self && !s_shape_warned && !filter_has_default_shape(filter)) {
        s_shape_warned = true;
        bb_log_w(TAG, "filter '%s' does not match the metrics/<hostname>/* shape "
                      "self-exclusion assumes; messages on it will NOT be self-excluded",
                 filter);
    }
    snprintf(s_topics[s_topic_count], sizeof(s_topics[s_topic_count]), "%s", filter);
    s_topic_count++;
    return BB_OK;
}

// Parse CONFIG_BB_SUB_MQTT_TOPICS (space/comma-separated) into individual
// filters. Only runs once; does nothing if bb_sub_mqtt_add_topic() was
// already called explicitly before this (caller-supplied filters win).
static void load_kconfig_default(void)
{
    if (s_kconfig_loaded) return;
    s_kconfig_loaded = true;
    if (s_topic_count > 0) return;

    const char *cfg = CONFIG_BB_SUB_MQTT_TOPICS;
#ifdef BB_SUB_MQTT_TESTING
    if (s_topics_cfg_override) cfg = s_topics_cfg_override;
#endif
    if (!cfg || !cfg[0]) return;  // LCOV_EXCL_BR_LINE — cfg is never NULL: macro fallback and override both guarantee a non-NULL string

    char buf[256];
    snprintf(buf, sizeof(buf), "%s", cfg);
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t,", &save); tok; tok = strtok_r(NULL, " \t,", &save)) {
        bb_sub_mqtt_add_topic(tok);
    }
}

// ---------------------------------------------------------------------------
// Self-exclusion (see bb_sub_mqtt.h header comment)
// ---------------------------------------------------------------------------

void bb_sub_mqtt_set_ignore_self(bool ignore_self)
{
    s_ignore_self = ignore_self;
}

// Returns true when topic's 2nd '/'-delimited segment equals hostname,
// i.e. "<prefix>/<hostname>/<subtopic>". Returns false (do not exclude) if
// the topic has fewer than 2 segments or hostname is empty.
static bool topic_is_own_hostname(const char *topic, const char *hostname)
{
    if (!topic || !hostname || !hostname[0]) return false;  // LCOV_EXCL_BR_LINE — topic is always non-NULL from bb_mqtt's callback; hostname is always non-NULL from bb_nv_config_hostname() (static buffer)

    const char *seg1 = strchr(topic, '/');
    if (!seg1) return false;
    seg1++;   // start of 2nd segment

    const char *seg1_end = strchr(seg1, '/');
    size_t seg1_len = seg1_end ? (size_t)(seg1_end - seg1) : strlen(seg1);

    size_t host_len = strlen(hostname);
    return seg1_len == host_len && strncmp(seg1, hostname, seg1_len) == 0;
}

// ---------------------------------------------------------------------------
// bb_mqtt receive callback
// ---------------------------------------------------------------------------

static void on_mqtt_message(const char *topic, const void *payload, size_t len, void *ctx)
{
    (void)ctx;

    if (s_ignore_self && topic_is_own_hostname(topic, bb_nv_config_hostname())) {
        bb_log_d(TAG, "ignoring self-published topic '%s'", topic);
        return;
    }

    bb_sub_route(topic, (const char *)payload, len);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bb_err_t bb_sub_mqtt_init(void)
{
    load_kconfig_default();

    bb_mqtt_t h = bb_mqtt_default();
    if (!h) {
        bb_log_w(TAG, "no default MQTT client; skipping subscribe (%d filter(s) configured, "
                      "reboot required after MQTT is configured to apply)",
                 s_topic_count);
        return BB_OK;
    }

    bb_mqtt_on_message(h, on_mqtt_message, NULL);

    for (int i = 0; i < s_topic_count; i++) {
        bb_err_t rc = bb_mqtt_subscribe(h, s_topics[i], 0);
        if (rc != BB_OK) {
            bb_log_w(TAG, "subscribe '%s' failed: %d", s_topics[i], rc);
        } else {
            bb_log_i(TAG, "subscribed '%s'", s_topics[i]);
        }
    }
    return BB_OK;
}

// ---------------------------------------------------------------------------
// bb_init regular-tier self-registration
// ---------------------------------------------------------------------------

#if CONFIG_BB_SUB_MQTT_AUTOREGISTER
static bb_err_t bb_sub_mqtt_regular_init(bb_http_handle_t server)
{
    (void)server;
    return bb_sub_mqtt_init();
}

BB_INIT_REGISTER(bb_sub_mqtt, bb_sub_mqtt_regular_init);
#endif

// ---------------------------------------------------------------------------
// Test hook
// ---------------------------------------------------------------------------

#ifdef BB_SUB_MQTT_TESTING
void bb_sub_mqtt_reset_for_test(void)
{
    s_topic_count        = 0;
    s_kconfig_loaded     = false;
    s_ignore_self        = BB_SUB_MQTT_IGNORE_SELF_DEFAULT;
    s_shape_warned       = false;
    s_topics_cfg_override = NULL;
    memset(s_topics, 0, sizeof(s_topics));
}

int bb_sub_mqtt_test_topic_count(void)
{
    return s_topic_count;
}

void bb_sub_mqtt_test_set_topics_cfg(const char *cfg)
{
    s_topics_cfg_override = cfg;
}
#endif
