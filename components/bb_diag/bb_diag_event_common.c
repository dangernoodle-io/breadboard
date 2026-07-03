// Pure (host-testable) serializer for the diag.boot bb_cache retained topic.
// No FreeRTOS or ESP-IDF types here.
#include "bb_diag_event_priv.h"
#include "bb_system.h"

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

    // Reboot-reason SSOT (B1-527 PR-A). Always present — source defaults to
    // "unknown" when no semantic record was captured (e.g. reset_reason
    // wasn't software, or this is the deploy boot).
    bb_json_t rr = bb_json_obj_new();
    if (rr) {
        bb_json_obj_set_string(rr, "source", bb_reset_source_str((bb_reset_source_t)s->reboot_src));
        if (s->reboot_detail[0] != '\0') {
            bb_json_obj_set_string(rr, "detail", s->reboot_detail);
        }
        bb_json_obj_set_int(rr, "uptime_s", (int64_t)s->reboot_uptime_s);
        bb_json_obj_set_int(rr, "epoch_s",  (int64_t)s->reboot_epoch_s);
        // age_s only makes sense when both the recorded epoch and the
        // current wall clock are known-good; a not-yet-synced "now" would
        // otherwise produce a bogus (huge or negative) age.
        if (s->reboot_epoch_s > 0 && s->now_epoch_valid && s->now_epoch_s >= s->reboot_epoch_s) {
            bb_json_obj_set_int(rr, "age_s", (int64_t)(s->now_epoch_s - s->reboot_epoch_s));
        }
        bb_json_obj_set_obj(obj, "reboot_reason", rr);
    }
}
