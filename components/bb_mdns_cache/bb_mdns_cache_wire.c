// bb_mdns_cache_wire — the format-agnostic "mdns_cache entry" descriptor
// SSOT + fill. See bb_mdns_cache_wire_priv.h for the wire-struct contract
// and the documented shape divergence from the legacy entry_serialize()
// emitter. Compiles on both host and ESP-IDF; no platform-specific code.

#include "bb_mdns_cache_wire_priv.h"

#include "bb_str.h"

#include <stddef.h>
#include <string.h>

const bb_serialize_field_t bb_mdns_cache_txt_wire_fields[2] = {
    { .key = "key", .type = BB_TYPE_STR,
      .offset = offsetof(bb_mdns_cache_txt_wire_t, key),
      .max_len = sizeof(((bb_mdns_cache_txt_wire_t *)0)->key) },
    { .key = "value", .type = BB_TYPE_STR,
      .offset = offsetof(bb_mdns_cache_txt_wire_t, value),
      .max_len = sizeof(((bb_mdns_cache_txt_wire_t *)0)->value) },
};

static const bb_serialize_field_t s_mdns_cache_entry_wire_fields[] = {
    { .key = "hostname", .type = BB_TYPE_STR,
      .offset = offsetof(bb_mdns_cache_entry_wire_t, hostname),
      .max_len = sizeof(((bb_mdns_cache_entry_wire_t *)0)->hostname) },
    { .key = "ip4", .type = BB_TYPE_STR,
      .offset = offsetof(bb_mdns_cache_entry_wire_t, ip4),
      .max_len = sizeof(((bb_mdns_cache_entry_wire_t *)0)->ip4) },
    { .key = "port", .type = BB_TYPE_I64,
      .offset = offsetof(bb_mdns_cache_entry_wire_t, port) },
    { .key = "txt", .type = BB_TYPE_ARR,
      .offset = offsetof(bb_mdns_cache_entry_wire_t, txt),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(bb_mdns_cache_txt_wire_t),
      .max_items = BB_MDNS_CACHE_WIRE_TXT_MAX,
      .children = bb_mdns_cache_txt_wire_fields,
      .n_children = 2 },
};

const bb_serialize_desc_t bb_mdns_cache_entry_wire_desc = {
    .type_name = "mdns_cache_entry",
    .fields    = s_mdns_cache_entry_wire_fields,
    .n_fields  = sizeof(s_mdns_cache_entry_wire_fields) / sizeof(s_mdns_cache_entry_wire_fields[0]),
    .snap_size = sizeof(bb_mdns_cache_entry_wire_t),
};

void bb_mdns_cache_entry_wire_fill(bb_mdns_cache_entry_wire_t *dst,
                                    const void *entry, size_t entry_size,
                                    const bb_mdns_txt_field_t *txt_fields,
                                    size_t txt_count, size_t *out_dropped)
{
    memset(dst, 0, sizeof(*dst));

    const bb_mdns_cache_entry_t *identity = (const bb_mdns_cache_entry_t *)entry;
    bb_strlcpy(dst->hostname, identity->hostname, sizeof(dst->hostname));
    bb_strlcpy(dst->ip4, identity->ip4, sizeof(dst->ip4));
    dst->port = (int64_t)identity->port;

    dst->txt.items = dst->txt_items;
    dst->txt.count = 0;

    size_t dropped = 0;

    if (!txt_fields || txt_count == 0) {
        if (out_dropped) *out_dropped = dropped;
        return;
    }

    const uint8_t *entry_bytes = (const uint8_t *)entry;
    size_t n = 0;
    for (size_t i = 0; i < txt_count; i++) {
        if (n >= BB_MDNS_CACHE_WIRE_TXT_MAX) {
            dropped++;
            continue;
        }

        const bb_mdns_txt_field_t *f = &txt_fields[i];
        if (!f->txt_key) continue;
        if (f->dest_offset + f->dest_len > entry_size) continue;

        bb_mdns_cache_txt_wire_t *row = &dst->txt_items[n];
        bb_strlcpy(row->key, f->txt_key, sizeof(row->key));
        // Bounded by min(field->dest_len, sizeof(row->value)) -- never just
        // sizeof(row->value) -- so this read never scans past the field's
        // own declared bound, independent of the external (unenforced-here)
        // NUL-within-dest_len guarantee bb_mdns_cache_apply_txt() provides.
        size_t value_cap = f->dest_len < sizeof(row->value) ? f->dest_len : sizeof(row->value);
        bb_strlcpy(row->value, (const char *)(entry_bytes + f->dest_offset), value_cap);
        n++;
    }
    dst->txt.count = n;
    if (out_dropped) *out_dropped = dropped;
}
