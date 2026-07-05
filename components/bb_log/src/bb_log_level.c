#include "bb_log.h"
#include "bb_log_internal.h"
#include "bb_str.h"

#include <ctype.h>
#include <string.h>

#ifdef CONFIG_BB_LOG_TAG_REGISTRY_MAX
#define BB_LOG_REGISTRY_MAX CONFIG_BB_LOG_TAG_REGISTRY_MAX
#else
#define BB_LOG_REGISTRY_MAX 32
#endif

// Compile-time guard: BB_LOG_LEVEL_LIST must have exactly one X() entry per
// bb_log_level_t enumerator (contiguous 0..BB_LOG_LEVEL_VERBOSE). Catches
// drift between the enum and the X-macro at build time instead of silently
// mis-mapping a level string.
#define BB_LOG_LEVEL_LIST_COUNT_ONE(v, s) +1
_Static_assert((0 BB_LOG_LEVEL_LIST(BB_LOG_LEVEL_LIST_COUNT_ONE)) == (BB_LOG_LEVEL_VERBOSE + 1),
               "BB_LOG_LEVEL_LIST entry count must match bb_log_level_t cardinality");
#undef BB_LOG_LEVEL_LIST_COUNT_ONE

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

    static const struct {
        bb_log_level_t level;
        const char *name;
    } s_levels[] = {
#define X(v, s) { v, s },
        BB_LOG_LEVEL_LIST(X)
#undef X
    };

    for (size_t j = 0; j < sizeof(s_levels) / sizeof(s_levels[0]); j++) {
        if (strcmp(buf, s_levels[j].name) == 0) {
            *out = s_levels[j].level;
            return true;
        }
    }

    return false;
}

const char *bb_log_level_to_str(bb_log_level_t level)
{
    switch (level) {
#define X(v, s) case v: return s;
        BB_LOG_LEVEL_LIST(X)
#undef X
        default: return "unknown";
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
            bb_strlcpy(s_registry[i].tag, tag, sizeof(s_registry[i].tag));
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
