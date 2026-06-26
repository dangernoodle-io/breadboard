// Pure (host-testable) serializer for the diag.boot bb_cache retained topic.
// No FreeRTOS or ESP-IDF types here.
#include "bb_diag_event_priv.h"

#include <string.h>

void bb_diag_boot_serialize(bb_json_t obj, const void *snap)
{
    const bb_diag_boot_snap_t *s = (const bb_diag_boot_snap_t *)snap;

    bb_json_obj_set_string(obj, "reset_reason", s->reset_reason);
    bb_json_obj_set_int   (obj, "wdt_resets",   (int64_t)s->wdt_resets);

    bb_json_t panic = bb_json_obj_new();
    if (panic) {
        bb_json_obj_set_bool(panic, "available", s->panic_available);
        if (s->panic_available) {
            bb_json_obj_set_int(panic, "boots_since", (int64_t)s->panic_boots_since);
        }
        bb_json_obj_set_obj(obj, "panic", panic);
    }

    bb_json_obj_set_bool(obj, "pending_verify", s->pending_verify);
    bb_json_obj_set_bool(obj, "rolled_back",    s->rolled_back);
}
