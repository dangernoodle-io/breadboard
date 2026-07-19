// bb_diag_storage_nvs -- see bb_diag_storage_nvs.h for the section contract.
// Pure/portable two-phase iter: bb_storage_list_entries()/
// bb_storage_get_stats() (PR5) merged by (ns_or_dir, key) with
// bb_settings_nv_overlay_entries() (PR7). BB_ARR_STREAM conversion (no
// fixed row-count cap): B1-1077 PR-2.

#include "bb_diag_storage_nvs.h"

#include "bb_mem.h"
#include "bb_settings.h"
#include "bb_storage.h"
#include "bb_str.h"

#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// ESP-IDF-system namespace denylist (exact-match, NOT a prefix match --
// "phy_extra" must not match "phy"). See bb_diag_storage_nvs.h's file
// header for the rationale: a bb-owned allowlist would need constant
// hand-maintenance (bb_tcp_client/bb_udp_client open per-instance-
// parameterized namespaces), so this denylist flags the two known
// ESP-IDF-owned namespaces instead.
// ---------------------------------------------------------------------------
static const char *const s_system_namespaces[] = {
    "nvs.net80211",  // esp_wifi_set_storage(WIFI_STORAGE_FLASH)
    "phy",           // esp_phy_init.c PHY_NAMESPACE calibration data
};

static bool is_system_namespace(const char *ns_or_dir)
{
    if (!ns_or_dir) return false;
    for (size_t i = 0; i < sizeof(s_system_namespaces) / sizeof(s_system_namespaces[0]); i++) {
        if (strcmp(ns_or_dir, s_system_namespaces[i]) == 0) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Static enc-name fallback table -- used as `type_str` for a live entry with
// no schema match. Static-storage-duration strings only (bb_serialize_str_n_t
// is a borrowed ptr+len -- never point it at a stack buffer).
// ---------------------------------------------------------------------------
static const char *const s_enc_names[] = {
    [BB_STORAGE_ENC_BLOB] = "blob",
    [BB_STORAGE_ENC_STR]  = "str",
    [BB_STORAGE_ENC_U8]   = "u8",
    [BB_STORAGE_ENC_U16]  = "u16",
    [BB_STORAGE_ENC_U32]  = "u32",
    [BB_STORAGE_ENC_I32]  = "i32",
};

static const char *enc_name(bb_storage_enc_t enc)
{
    size_t idx = (size_t)enc;
    if (idx >= sizeof(s_enc_names) / sizeof(s_enc_names[0]) || !s_enc_names[idx]) return "blob";
    return s_enc_names[idx];
}

// ---------------------------------------------------------------------------
// Descriptor
// ---------------------------------------------------------------------------

static const bb_serialize_field_t s_row_fields[] = {
    { .key = "ns_or_dir", .type = BB_TYPE_STR,
      .offset = offsetof(bb_diag_storage_nvs_row_t, ns_or_dir),
      .max_len = sizeof(((bb_diag_storage_nvs_row_t *)0)->ns_or_dir) },
    { .key = "key", .type = BB_TYPE_STR,
      .offset = offsetof(bb_diag_storage_nvs_row_t, key),
      .max_len = sizeof(((bb_diag_storage_nvs_row_t *)0)->key) },
    { .key = "type", .type = BB_TYPE_STR_N,
      .offset = offsetof(bb_diag_storage_nvs_row_t, type_str) },
    { .key = "label", .type = BB_TYPE_STR_N,
      .offset = offsetof(bb_diag_storage_nvs_row_t, label) },
    { .key = "len", .type = BB_TYPE_U64,
      .offset = offsetof(bb_diag_storage_nvs_row_t, len) },
    { .key = "system", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_diag_storage_nvs_row_t, system) },
    { .key = "has_schema", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_diag_storage_nvs_row_t, has_schema) },
    { .key = "secret", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_diag_storage_nvs_row_t, secret) },
    { .key = "provisioning_only", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_diag_storage_nvs_row_t, provisioning_only) },
    { .key = "reboot_required", .type = BB_TYPE_BOOL,
      .offset = offsetof(bb_diag_storage_nvs_row_t, reboot_required) },
};

static const bb_serialize_field_t s_snap_fields[] = {
    { .key = "entries", .type = BB_TYPE_ARR,
      .offset = offsetof(bb_diag_storage_nvs_snap_t, entries),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(bb_diag_storage_nvs_row_t),
      .cardinality = BB_ARR_STREAM,
      .children = s_row_fields,
      .n_children = sizeof(s_row_fields) / sizeof(s_row_fields[0]) },
    { .key = "entry_count", .type = BB_TYPE_U64,
      .offset = offsetof(bb_diag_storage_nvs_snap_t, entry_count) },
    { .key = "used_bytes", .type = BB_TYPE_U64,
      .offset = offsetof(bb_diag_storage_nvs_snap_t, used_bytes) },
    { .key = "free_bytes", .type = BB_TYPE_U64,
      .offset = offsetof(bb_diag_storage_nvs_snap_t, free_bytes) },
    { .key = "total_bytes", .type = BB_TYPE_U64,
      .offset = offsetof(bb_diag_storage_nvs_snap_t, total_bytes) },
    { .key = "namespace_count", .type = BB_TYPE_U64,
      .offset = offsetof(bb_diag_storage_nvs_snap_t, namespace_count) },
};

const bb_serialize_desc_t bb_diag_storage_nvs_desc = {
    .type_name = "storage_nvs",
    .fields    = s_snap_fields,
    .n_fields  = sizeof(s_snap_fields) / sizeof(s_snap_fields[0]),
    .snap_size = sizeof(bb_diag_storage_nvs_snap_t),
};

// ---------------------------------------------------------------------------
// Fill
// ---------------------------------------------------------------------------

static const bb_settings_nv_overlay_entry_t *find_overlay_match(
    const bb_settings_nv_overlay_entry_t *overlay, size_t overlay_count,
    const char *ns_or_dir, const char *key)
{
    for (size_t i = 0; i < overlay_count; i++) {
        if (strcmp(overlay[i].ns_or_dir, ns_or_dir) == 0 && strcmp(overlay[i].key, key) == 0) {
            return &overlay[i];
        }
    }
    return NULL;
}

// Composes one bb_diag_storage_nvs_row_t from a live bb_storage_entry_t plus
// the schema overlay -- the exact merge logic the prior fixed-cap fill()
// used, factored out so phase 2 below stays a plain per-entry loop.
static void compose_row(bb_diag_storage_nvs_row_t *row, const bb_storage_entry_t *live,
                         const bb_settings_nv_overlay_entry_t *overlay, size_t overlay_count)
{
    memset(row, 0, sizeof(*row));

    bb_strlcpy(row->ns_or_dir, live->ns_or_dir, sizeof(row->ns_or_dir));
    bb_strlcpy(row->key, live->key, sizeof(row->key));
    row->len    = (uint64_t)live->len;
    row->system = is_system_namespace(row->ns_or_dir);

    const bb_settings_nv_overlay_entry_t *match =
        find_overlay_match(overlay, overlay_count, row->ns_or_dir, row->key);
    if (match) {
        row->has_schema        = true;
        row->type_str.ptr      = match->type_str;
        row->type_str.len      = strlen(match->type_str);
        row->label.ptr         = match->label;
        row->label.len         = strlen(match->label);
        row->secret            = match->secret;
        row->provisioning_only = match->provisioning_only;
        row->reboot_required   = match->reboot_required;
    } else {
        const char *fallback = enc_name(live->enc);
        row->has_schema   = false;
        row->type_str.ptr = fallback;
        row->type_str.len = strlen(fallback);
        row->label.ptr    = NULL;
        row->label.len    = 0;
    }
}

bb_err_t bb_diag_storage_nvs_iter(void *dst, void *row_arena, size_t row_cap,
                                   size_t *row_count, const bb_diag_fill_args_t *args)
{
    (void)args;
    if (!dst || !row_count) return BB_ERR_INVALID_ARG;

    bb_diag_storage_nvs_snap_t *snap = (bb_diag_storage_nvs_snap_t *)dst;
    memset(snap, 0, sizeof(*snap));

    size_t   live_total = 0;
    bb_err_t rc = bb_storage_list_entries("nvs", NULL, NULL, 0, &live_total);
    if (rc != BB_OK) return rc;

    bb_storage_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    rc = bb_storage_get_stats("nvs", &stats);
    if (rc != BB_OK) return rc;

    snap->entry_count     = (uint64_t)live_total;
    snap->used_bytes      = (uint64_t)stats.used_bytes;
    snap->free_bytes      = (uint64_t)stats.free_bytes;
    snap->total_bytes     = (uint64_t)stats.total_bytes;
    snap->namespace_count = (uint64_t)stats.namespace_count;

    if (!row_arena || row_cap == 0) {
        // Phase 1 (count-only) -- no arena yet, wire an empty stream so a
        // direct (non-dispatcher) caller of a phase-1-only `dst` still
        // renders `entries:[]` rather than reading an unwired carrier.
        *row_count = live_total;
        snap->entries = bb_serialize_arr_stream_from_buf(&snap->entries_iter_state, NULL, 0,
                                                           sizeof(bb_diag_storage_nvs_row_t));
        return BB_OK;
    }

    // Phase 2: re-enumerate live entries into a section-owned temporary
    // staging buffer (distinct from `row_arena`, which the DISPATCHER owns
    // and frees -- see bb_diag_section_dispatch.c) bounded by `row_cap`
    // (phase 1's reported count), then compose each into `row_arena[i]`.
    bb_storage_entry_t *live = bb_malloc_prefer_spiram(row_cap * sizeof(bb_storage_entry_t));
    if (!live) return BB_ERR_NO_MEM;

    size_t live_n = 0;
    rc = bb_storage_list_entries("nvs", NULL, live, row_cap, &live_n);
    if (rc != BB_OK) {
        bb_mem_free(live);
        return rc;
    }

    bb_settings_nv_overlay_entry_t overlay[BB_SETTINGS_NV_OVERLAY_CAP];
    size_t overlay_total = bb_settings_nv_overlay_entries(overlay, BB_SETTINGS_NV_OVERLAY_CAP);
    size_t overlay_count = overlay_total < BB_SETTINGS_NV_OVERLAY_CAP ? overlay_total : BB_SETTINGS_NV_OVERLAY_CAP;

    size_t n = live_n < row_cap ? live_n : row_cap;
    bb_diag_storage_nvs_row_t *rows = (bb_diag_storage_nvs_row_t *)row_arena;
    for (size_t i = 0; i < n; i++) {
        compose_row(&rows[i], &live[i], overlay, overlay_count);
    }

    bb_mem_free(live);

    *row_count     = n;
    snap->entries = bb_serialize_arr_stream_from_buf(&snap->entries_iter_state, row_arena, n,
                                                       sizeof(bb_diag_storage_nvs_row_t));
    return BB_OK;
}

#ifdef ESP_PLATFORM
bb_err_t bb_diag_storage_nvs_register(void)
{
    bb_diag_section_t section = {
        .name         = "storage/nvs",
        .desc         = "NVS storage inventory (live entries + schema overlay)",
        .snap_desc    = &bb_diag_storage_nvs_desc,
        .iter         = bb_diag_storage_nvs_iter,
        .ctx          = NULL,
        .query_keys   = NULL,
        .n_query_keys = 0,
    };
    return bb_diag_register_section(&section);
}
#endif
