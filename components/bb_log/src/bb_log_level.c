#include "bb_log.h"
#include "bb_log_internal.h"

#include <ctype.h>
#include <string.h>

#define BB_LOG_REGISTRY_MAX 32

typedef struct {
    char tag[32];
    bb_log_level_t level;
    bool used;
} s_registry_entry_t;

static s_registry_entry_t s_registry[BB_LOG_REGISTRY_MAX] = {0};

bool bb_log_level_from_str(const char *s, bb_log_level_t *out)
{
    if (!s || !out) return false;

    char buf[16];
    size_t i;
    for (i = 0; i < sizeof(buf) - 1 && s[i]; i++) {
        buf[i] = tolower((unsigned char)s[i]);
    }
    buf[i] = '\0';

    if (strcmp(buf, "none") == 0) {
        *out = BB_LOG_LEVEL_NONE;
        return true;
    }
    if (strcmp(buf, "error") == 0) {
        *out = BB_LOG_LEVEL_ERROR;
        return true;
    }
    if (strcmp(buf, "warn") == 0) {
        *out = BB_LOG_LEVEL_WARN;
        return true;
    }
    if (strcmp(buf, "info") == 0) {
        *out = BB_LOG_LEVEL_INFO;
        return true;
    }
    if (strcmp(buf, "debug") == 0) {
        *out = BB_LOG_LEVEL_DEBUG;
        return true;
    }
    if (strcmp(buf, "verbose") == 0) {
        *out = BB_LOG_LEVEL_VERBOSE;
        return true;
    }

    return false;
}

const char *bb_log_level_to_str(bb_log_level_t level)
{
    switch (level) {
        case BB_LOG_LEVEL_NONE:    return "none";
        case BB_LOG_LEVEL_ERROR:   return "error";
        case BB_LOG_LEVEL_WARN:    return "warn";
        case BB_LOG_LEVEL_INFO:    return "info";
        case BB_LOG_LEVEL_DEBUG:   return "debug";
        case BB_LOG_LEVEL_VERBOSE: return "verbose";
        default:                   return "unknown";
    }
}

static int _find_registry_entry(const char *tag)
{
    for (int i = 0; i < BB_LOG_REGISTRY_MAX; i++) {
        if (s_registry[i].used && strcmp(s_registry[i].tag, tag) == 0) {
            return i;
        }
    }
    return -1;
}

static void _bb_log_registry_set(const char *tag, bb_log_level_t level)
{
    int idx = _find_registry_entry(tag);
    if (idx >= 0) {
        // Update existing entry
        s_registry[idx].level = level;
        return;
    }

    // Find first free slot
    for (int i = 0; i < BB_LOG_REGISTRY_MAX; i++) {
        if (!s_registry[i].used) {
            strncpy(s_registry[i].tag, tag, sizeof(s_registry[i].tag) - 1);
            s_registry[i].tag[sizeof(s_registry[i].tag) - 1] = '\0';
            s_registry[i].level = level;
            s_registry[i].used = true;
            return;
        }
    }
    // Registry full; silently drop
}

void bb_log_level_set(const char *tag, bb_log_level_t level)
{
    if (!tag) return;
    _bb_log_registry_set(tag, level);
    _bb_log_level_set_backend(tag, level);
}

void bb_log_tag_register(const char *tag, bb_log_level_t level)
{
    if (!tag) return;

    // Only register if tag not already present
    if (_find_registry_entry(tag) >= 0) {
        return; // Idempotent: already registered, no-op
    }

    _bb_log_registry_set(tag, level);
    _bb_log_level_set_backend(tag, level);
}

bool bb_log_tag_level(const char *tag, bb_log_level_t *out)
{
    if (!tag || !out) return false;

    int idx = _find_registry_entry(tag);
    if (idx < 0) return false;

    *out = s_registry[idx].level;
    return true;
}

bool bb_log_tag_at(size_t index, const char **tag_out, bb_log_level_t *level_out)
{
    if (!tag_out || !level_out) return false;

    size_t count = 0;
    for (int i = 0; i < BB_LOG_REGISTRY_MAX; i++) {
        if (s_registry[i].used) {
            if (count == index) {
                *tag_out = s_registry[i].tag;
                *level_out = s_registry[i].level;
                return true;
            }
            count++;
        }
    }
    return false;
}

#ifndef ESP_PLATFORM
// Host-only: reset registry for testing
void _bb_log_registry_reset(void)
{
    for (int i = 0; i < BB_LOG_REGISTRY_MAX; i++) {
        s_registry[i].used = false;
        s_registry[i].tag[0] = '\0';
        s_registry[i].level = BB_LOG_LEVEL_INFO;
    }
}
#endif
