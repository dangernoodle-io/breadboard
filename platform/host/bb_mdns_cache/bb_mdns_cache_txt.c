// bb_mdns_cache — consumer-configured TXT capture, pure byte-copy + bb_json,
// no locks/clock/I/O. Compiled on host and ESP-IDF; called verbatim from the
// ESP-IDF glue (hello/bye handlers + re-query worker) and by host tests.

#include "bb_mdns_cache.h"
#include "bb_str.h"

#include <string.h>

void bb_mdns_cache_apply_txt(void *entry, size_t entry_size,
                              const bb_mdns_txt_field_t *fields, size_t field_count,
                              const bb_mdns_txt_t *txt, size_t txt_count)
{
    if (!entry || entry_size == 0) return;
    if (!fields || field_count == 0) return;
    if (!txt || txt_count == 0) return;

    for (size_t i = 0; i < field_count; i++) {
        const bb_mdns_txt_field_t *f = &fields[i];
        if (!f->txt_key || f->dest_len == 0) continue;
        if (f->dest_offset + f->dest_len > entry_size) continue;

        for (size_t j = 0; j < txt_count; j++) {
            if (!txt[j].key || strcmp(txt[j].key, f->txt_key) != 0) continue;
            char *dest = (char *)entry + f->dest_offset;
            bb_strlcpy(dest, txt[j].value ? txt[j].value : "", f->dest_len);
            break; // first match in txt[] order wins on duplicate keys
        }
    }
}

void bb_mdns_cache_txt_serialize(bb_json_t obj, const void *entry, size_t entry_size,
                                  const bb_mdns_txt_field_t *fields, size_t field_count)
{
    if (!obj || !entry || !fields || field_count == 0) return;

    for (size_t i = 0; i < field_count; i++) {
        const bb_mdns_txt_field_t *f = &fields[i];
        if (!f->txt_key) continue;
        if (f->dest_offset + f->dest_len > entry_size) continue;
        const char *value = (const char *)entry + f->dest_offset;
        bb_json_obj_set_string(obj, f->txt_key, value);
    }
}
