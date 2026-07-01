// bb_sink_event — bb_pub sink that forwards telemetry payloads to bb_event topics.
// Compiled on both host (tests) and ESP-IDF.
#include "bb_sink_event.h"
#include "bb_event.h"
#include "bb_event_routes.h"
#include "bb_json.h"
#include "bb_log.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "bb_sink_event";

// ---------------------------------------------------------------------------
// Internal topic registry
// ---------------------------------------------------------------------------

typedef struct {
    char              subtopic[64];
    bb_event_topic_t  handle;
    bool              retained;
} sink_event_entry_t;

static sink_event_entry_t s_entries[BB_SINK_EVENT_MAX_TOPICS];
static int                s_count = 0;

// ---------------------------------------------------------------------------
// Publish callback
// ---------------------------------------------------------------------------

static bb_err_t sink_event_publish(void *ctx, const char *topic,
                                   const char *payload, int len, bool retain)
{
    (void)ctx;
    (void)retain;   /* event bus has no retain concept */
    // Extract subtopic: find the part after the 2nd '/' in the topic string.
    // Topic format: "<prefix>/<hostname>/<subtopic>"
    const char *p = topic;
    int slashes = 0;
    while (*p && slashes < 2) {
        if (*p == '/') slashes++;
        p++;
    }
    const char *subtopic = p;

    // Look up in registry.
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].subtopic, subtopic) == 0) {
            bb_err_t err = bb_event_post(s_entries[i].handle, 0,
                                         payload, (size_t)len);
            if (err != BB_OK) {
                bb_log_w(TAG, "post '%s' failed: %d", subtopic, (int)err);
            }
            return err;
        }
    }
    // Not registered: silently skip.
    bb_log_d(TAG, "publish: subtopic '%s' not registered, skipping", subtopic);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_sink_event_register_topic(const char *subtopic, bool retained)
{
    if (!subtopic || subtopic[0] == '\0') return BB_ERR_INVALID_ARG;
    if (s_count >= BB_SINK_EVENT_MAX_TOPICS) return BB_ERR_NO_SPACE;

    sink_event_entry_t *entry = &s_entries[s_count];
    strncpy(entry->subtopic, subtopic, sizeof(entry->subtopic) - 1);
    entry->subtopic[sizeof(entry->subtopic) - 1] = '\0';
    entry->retained = retained;

    bb_err_t err = bb_event_topic_register(subtopic, &entry->handle);
    if (err != BB_OK) {
        bb_log_w(TAG, "topic_register '%s' failed: %d", subtopic, (int)err);
        return err;
    }
    err = bb_event_routes_attach_ex(subtopic, retained);
    if (err != BB_OK) {
        bb_log_w(TAG, "routes_attach_ex '%s' failed: %d", subtopic, (int)err);
        return err;
    }
    s_count++;
    return BB_OK;
}

bb_err_t bb_sink_event(bb_pub_sink_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    out->publish       = sink_event_publish;
    out->ctx           = NULL;
    out->transport     = NULL;
    out->tls           = false;
    out->subscribe     = NULL;
    out->subscribe_ctx = NULL;
    return BB_OK;
}

void bb_sink_event_seed_all(void)
{
    for (int i = 0; i < s_count; i++) {
        bb_json_t obj = bb_json_obj_new();
        if (!obj) {
            bb_log_w(TAG, "seed_all: failed to allocate JSON obj for '%s'",
                     s_entries[i].subtopic);
            continue;
        }
        bool ok = bb_pub_sample_into(s_entries[i].subtopic, obj);
        if (ok) {
            char *s = bb_json_serialize(obj);
            if (s) {
                bb_event_post(s_entries[i].handle, 0, s, strlen(s));
                bb_json_free_str(s);
            }
        }
        bb_json_free(obj);
    }
}

#ifdef BB_SINK_EVENT_TESTING
void bb_sink_event_reset_for_test(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_count = 0;
}
#endif
