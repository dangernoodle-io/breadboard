// Portable namespace registry + pure dispatch helper for the bb_http_section
// helper (bb_http_section PR). See bb_http_section.h for the public seam
// contract and bb_http_section_priv.h for the internals shared with the
// ESP-IDF dispatcher and host tests. A small fixed array + linear scan
// (mirrors bb_diag_section.c's own bb_registry-backed table, except lookup
// here is a longest-prefix-match, not an exact-key lookup, so bb_registry's
// exact-match API doesn't fit -- the table is small enough (
// BB_HTTP_SECTION_TABLE_CAP) that a linear scan is the right tool).
#include "bb_http_section_priv.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    bool                  in_use;
    char                  prefix[BB_HTTP_SECTION_PREFIX_MAX];
    char                  wildcard[BB_HTTP_SECTION_PREFIX_MAX + 1];  // "<prefix>*"
    bb_http_section_ns_t  ns;
} bb_http_section_slot_t;

static bb_http_section_slot_t s_slots[BB_HTTP_SECTION_TABLE_CAP];
static size_t                 s_count = 0;

// Counts non-empty '/'-delimited path segments in `prefix` (e.g. "/api/" ->
// 1 segment "api"; "/api/sensors/" -> 2 segments "api"/"sensors"). Used by
// bb_http_section_register_ns()'s minimum-segment-depth guard (see
// bb_http_section.h's own doc) -- rejects a namespace prefix as broad as
// "/api/" itself, exactly the blanket-shadowing shape this dispatcher
// deliberately avoids.
static size_t count_path_segments(const char *prefix)
{
    size_t segments = 0;
    bool   in_segment = false;
    for (const char *p = prefix; *p; p++) {
        if (*p == '/') {
            in_segment = false;
        } else if (!in_segment) {
            in_segment = true;
            segments++;
        }
    }
    return segments;
}

bb_err_t bb_http_section_register_ns(const bb_http_section_ns_t *ns)
{
    if (!ns || !ns->prefix) return BB_ERR_INVALID_ARG;
    if (!ns->render && !ns->apply) return BB_ERR_INVALID_ARG;
    if (strlen(ns->prefix) >= BB_HTTP_SECTION_PREFIX_MAX) return BB_ERR_INVALID_ARG;
    if (count_path_segments(ns->prefix) < 2) return BB_ERR_INVALID_ARG;

    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_slots[i].prefix, ns->prefix) == 0) return BB_ERR_INVALID_STATE;
    }

    if (s_count >= BB_HTTP_SECTION_TABLE_CAP) return BB_ERR_NO_SPACE;

    bb_http_section_slot_t *slot = &s_slots[s_count];
    strncpy(slot->prefix, ns->prefix, sizeof(slot->prefix) - 1);
    slot->prefix[sizeof(slot->prefix) - 1] = '\0';
    snprintf(slot->wildcard, sizeof(slot->wildcard), "%s*", slot->prefix);
    slot->ns        = *ns;
    slot->ns.prefix = slot->prefix;
    slot->in_use    = true;
    s_count++;

    return BB_OK;
}

size_t bb_http_section_count(void)
{
    return s_count;
}

const bb_http_section_ns_t *bb_http_section_at(size_t idx, const char **out_wildcard)
{
    if (idx >= s_count) return NULL;
    if (out_wildcard) *out_wildcard = s_slots[idx].wildcard;
    return &s_slots[idx].ns;
}

const bb_http_section_ns_t *bb_http_section_find(const char *uri, char *out, size_t out_cap)
{
    if (!uri || !out || out_cap == 0) return NULL;

    const bb_http_section_slot_t *best     = NULL;
    size_t                        best_len = 0;

    for (size_t i = 0; i < s_count; i++) {
        const bb_http_section_slot_t *slot = &s_slots[i];
        size_t                        len  = strlen(slot->prefix);
        if (len <= best_len) continue;  // only a strictly-longer match can win
        if (strncmp(uri, slot->prefix, len) != 0) continue;
        best     = slot;
        best_len = len;
    }

    if (!best) return NULL;

    strncpy(out, uri + best_len, out_cap - 1);
    out[out_cap - 1] = '\0';
    return &best->ns;
}

#ifdef BB_HTTP_SECTION_TESTING
void bb_http_section_test_reset(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_count = 0;
}
#endif
