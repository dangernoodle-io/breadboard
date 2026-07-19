// bb_diag_storage_nvs -- see bb_diag_storage_nvs.h for the section contract.
// Pure/portable fill: bb_storage_list_entries()/bb_storage_get_stats() (PR5)
// merged by (ns_or_dir, key) with bb_settings_nv_overlay_entries() (PR7).

#include "bb_diag_storage_nvs.h"

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
      .max_items = BB_DIAG_STORAGE_NVS_ENTRY_CAP,
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

bb_err_t bb_diag_storage_nvs_fill(void *dst, const bb_diag_fill_args_t *args)
{
    (void)args;
    if (!dst) return BB_ERR_INVALID_ARG;

    bb_diag_storage_nvs_snap_t *snap = (bb_diag_storage_nvs_snap_t *)dst;
    memset(snap, 0, sizeof(*snap));

    bb_storage_entry_t live[BB_DIAG_STORAGE_NVS_ENTRY_CAP];
    size_t             live_total = 0;
    bb_err_t rc = bb_storage_list_entries("nvs", NULL, live, BB_DIAG_STORAGE_NVS_ENTRY_CAP, &live_total);
    if (rc != BB_OK) return rc;

    bb_storage_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    rc = bb_storage_get_stats("nvs", &stats);
    if (rc != BB_OK) return rc;

    bb_settings_nv_overlay_entry_t overlay[BB_SETTINGS_NV_OVERLAY_CAP];
    size_t overlay_total = bb_settings_nv_overlay_entries(overlay, BB_SETTINGS_NV_OVERLAY_CAP);
    size_t overlay_count = overlay_total < BB_SETTINGS_NV_OVERLAY_CAP ? overlay_total : BB_SETTINGS_NV_OVERLAY_CAP;

    size_t n = live_total < BB_DIAG_STORAGE_NVS_ENTRY_CAP ? live_total : BB_DIAG_STORAGE_NVS_ENTRY_CAP;
    for (size_t i = 0; i < n; i++) {
        bb_diag_storage_nvs_row_t *row = &snap->entries_items[i];

        bb_strlcpy(row->ns_or_dir, live[i].ns_or_dir, sizeof(row->ns_or_dir));
        bb_strlcpy(row->key, live[i].key, sizeof(row->key));
        row->len    = (uint64_t)live[i].len;
        row->system = is_system_namespace(row->ns_or_dir);

        const bb_settings_nv_overlay_entry_t *match =
            find_overlay_match(overlay, overlay_count, row->ns_or_dir, row->key);
        if (match) {
            row->has_schema         = true;
            row->type_str.ptr       = match->type_str;
            row->type_str.len       = strlen(match->type_str);
            row->label.ptr          = match->label;
            row->label.len          = strlen(match->label);
            row->secret             = match->secret;
            row->provisioning_only  = match->provisioning_only;
            row->reboot_required    = match->reboot_required;
        } else {
            const char *fallback = enc_name(live[i].enc);
            row->has_schema   = false;
            row->type_str.ptr = fallback;
            row->type_str.len = strlen(fallback);
            row->label.ptr    = NULL;
            row->label.len    = 0;
        }
    }

    snap->entries.items = snap->entries_items;
    snap->entries.count = n;
    snap->entry_count   = (uint64_t)live_total;

    snap->used_bytes      = (uint64_t)stats.used_bytes;
    snap->free_bytes      = (uint64_t)stats.free_bytes;
    snap->total_bytes     = (uint64_t)stats.total_bytes;
    snap->namespace_count = (uint64_t)stats.namespace_count;

    return BB_OK;
}

#ifdef ESP_PLATFORM
bb_err_t bb_diag_storage_nvs_register(void)
{
    bb_diag_section_t section = {
        .name         = "storage/nvs",
        .desc         = "NVS storage inventory (live entries + schema overlay)",
        .snap_desc    = &bb_diag_storage_nvs_desc,
        .fill         = bb_diag_storage_nvs_fill,
        .ctx          = NULL,
        .query_keys   = NULL,
        .n_query_keys = 0,
    };
    return bb_diag_register_section(&section);
}
#endif
