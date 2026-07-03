#pragma once

// bb_sub_mqtt — MQTT ingress adapter for bb_sub (B1-490).
//
// Subscribes to a configurable MQTT topic filter and routes every received
// message into bb_sub_route(). Self-registers at the regular bb_init tier
// (CONFIG_BB_SUB_MQTT_AUTOREGISTER, default y) once bb_mqtt_default() is
// resolvable (an MQTT client must already be configured/auto-registered).
//
// Default filter: "metrics/+/meta" (single-level wildcard), overridable via
// CONFIG_BB_SUB_MQTT_TOPICS (Kconfig string, space/comma-separated) or
// bb_sub_mqtt_add_topic() (call BEFORE bb_init_init() runs so the filter
// list is complete when the regular-tier init subscribes).
//
// Self-exclusion: a consumer that ALSO publishes into the same topic space
// it subscribes to (e.g. this device publishes "metrics/<hostname>/meta"
// and also subscribes to the wildcard "metrics/+/meta") would otherwise
// ingest its own published messages back through bb_sub_route(). Assuming
// the "<prefix>/<hostname>/<subtopic>" convention (matches bb_pub's own
// topic shape), bb_sub_mqtt drops any inbound message whose 2nd '/'-
// delimited segment equals bb_nv_config_hostname() — DEFAULT ON. Disable
// via CONFIG_BB_SUB_MQTT_INGEST_SELF=y (opt-in to ingest self) or override
// at runtime with bb_sub_mqtt_set_ignore_self(false).
//
// SHAPE ASSUMPTION: self-exclusion assumes the hostname sits at segment
// index 1 of the "metrics/<hostname>/*" shape. A filter added via
// bb_sub_mqtt_add_topic() (or CONFIG_BB_SUB_MQTT_TOPICS) whose shape does
// NOT put a hostname/wildcard at index 1 (e.g. a single-segment or
// non-standard filter) is NOT self-excluded — messages on it always route
// through, even from this device's own hostname. bb_sub_mqtt_add_topic()
// emits a one-time bb_log_w when a non-default-shaped filter is added while
// ignore_self is on, so this is diagnosable at boot. Per-filter segment
// declaration (an explicit "which segment is the hostname" override per
// filter) is a possible follow-up if a consumer needs a differently-shaped
// self-excluded filter; not implemented today.
//
// FAIL-OPEN: if bb_nv_config_hostname() is empty (hostname not yet
// configured), self-exclusion is a no-op — every message routes through,
// including ones this device may have published itself. This only matters
// during the brief window before hostname is set; treat it as fail-open,
// not fail-closed.
//
// THREADING CONTRACT: bb_sub_mqtt_set_ignore_self() and
// bb_sub_mqtt_add_topic() are boot-time, single-threaded configuration
// calls — they MUST be called before bb_sub_mqtt_init() (i.e. before
// bb_init_init() on the autoregistered path). There is no lock guarding
// s_topics/s_topic_count/s_ignore_self against the MQTT event task that
// later reads them from on_mqtt_message(); this is intentional (boot-time
// config, word-sized reads) — do not call these setters after init.

#include "bb_core.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BB_SUB_MQTT_MAX_TOPICS
#define BB_SUB_MQTT_MAX_TOPICS 8
#endif

/**
 * Add an MQTT topic filter to subscribe to (in addition to any filters
 * loaded from CONFIG_BB_SUB_MQTT_TOPICS). Must be called before the regular
 * bb_init tier runs (i.e. before bb_init_init()) to take effect at boot via
 * the autoregistered path; safe to call at any time when driving
 * bb_sub_mqtt_init() manually (CONFIG_BB_SUB_MQTT_AUTOREGISTER=n).
 *
 * @return BB_OK on success. BB_ERR_INVALID_ARG if filter is NULL/empty.
 *         BB_ERR_NO_SPACE if BB_SUB_MQTT_MAX_TOPICS filters are already set.
 */
bb_err_t bb_sub_mqtt_add_topic(const char *filter);

/**
 * Subscribe (via bb_mqtt_subscribe) to every configured topic filter and
 * register bb_sub_route (via bb_mqtt_on_message) as the bb_mqtt receive
 * callback. No-op (returns BB_OK, logs) if bb_mqtt_default() is NULL (no
 * MQTT client configured) — does not fail boot.
 *
 * Idempotent-ish: safe to call more than once; re-subscribes the same
 * filters (esp-mqtt treats a duplicate SUBSCRIBE as a QoS re-negotiation,
 * not an error).
 */
bb_err_t bb_sub_mqtt_init(void);

/**
 * Override the self-exclusion behavior (see header comment above).
 * Default true (drop own-hostname topics) unless
 * CONFIG_BB_SUB_MQTT_INGEST_SELF=y at build time.
 */
void bb_sub_mqtt_set_ignore_self(bool ignore_self);

#ifdef BB_SUB_MQTT_TESTING
/**
 * Reset bb_sub_mqtt's internal state (topic filter list, Kconfig-default
 * load latch, ignore-self flag back to its build-time default) — test
 * isolation. Does NOT touch bb_mqtt or bb_sub state; reset those
 * separately.
 */
void bb_sub_mqtt_reset_for_test(void);

/** Number of topic filters currently registered (s_topic_count). */
int bb_sub_mqtt_test_topic_count(void);

/**
 * Override the string load_kconfig_default() parses in place of
 * CONFIG_BB_SUB_MQTT_TOPICS, for exercising the comma/space strtok_r parse
 * with a multi-filter string without a Kconfig rebuild. Pass NULL to revert
 * to CONFIG_BB_SUB_MQTT_TOPICS. Must be called before bb_sub_mqtt_init()
 * (same boot-time-only contract as bb_sub_mqtt_add_topic()).
 */
void bb_sub_mqtt_test_set_topics_cfg(const char *cfg);

/**
 * Reset the lazily-registered bb_transport_health "mqtt_sub" INFERRED slot
 * handle back to BB_TRANSPORT_HANDLE_INVALID — test isolation. Does NOT
 * reset bb_transport_health's own state; call
 * bb_transport_health_reset_for_test() separately.
 */
void bb_sub_mqtt_reset_transport_health_for_test(void);
#endif

#ifdef __cplusplus
}
#endif
