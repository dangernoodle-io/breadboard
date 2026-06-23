#pragma once
#include <stdbool.h>
#include "bb_core.h"
#include "bb_pub.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#ifdef CONFIG_BB_SINK_EVENT_MAX_TOPICS
#define BB_SINK_EVENT_MAX_TOPICS CONFIG_BB_SINK_EVENT_MAX_TOPICS
#endif
#endif
#ifndef BB_SINK_EVENT_MAX_TOPICS
#define BB_SINK_EVENT_MAX_TOPICS 16
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Fill `out` with a bb_pub_sink_t that, for each published topic, strips
 * the <prefix>/<host>/ prefix to recover the subtopic, looks it up in the
 * pre-registered topic table, and calls bb_event_post. Subtopics not
 * registered are silently skipped.
 *
 * Usage:
 *   bb_sink_event_register_topic("power", true);
 *   bb_pub_sink_t s;
 *   bb_sink_event(&s);
 *   bb_pub_add_sink(&s);
 *   bb_sink_event_seed_all();
 */
bb_err_t bb_sink_event(bb_pub_sink_t *out);

/**
 * Register a subtopic for bb_sink_event delivery. Creates a bb_event topic
 * named `subtopic` and attaches it to bb_event_routes with the given retained
 * flag. Must be called before bb_sink_event() and bb_pub_add_sink().
 *
 * @return BB_OK, BB_ERR_NO_SPACE if max topics reached, BB_ERR_INVALID_ARG
 *         if subtopic is NULL or empty.
 */
bb_err_t bb_sink_event_register_topic(const char *subtopic, bool retained);

/**
 * Post one snapshot per registered topic so retained rings are non-empty from
 * boot. For each registered subtopic, calls bb_pub_sample_into to get the
 * payload, serializes it, and posts to the topic's bb_event_topic_t.
 * Topics with no matching bb_pub source are skipped.
 */
void bb_sink_event_seed_all(void);

#ifdef BB_SINK_EVENT_TESTING
/** Reset internal state for test isolation. */
void bb_sink_event_reset_for_test(void);
#endif

#ifdef __cplusplus
}
#endif
