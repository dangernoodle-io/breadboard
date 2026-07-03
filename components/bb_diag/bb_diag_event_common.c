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

    // Rolling reboot history (B1-527 PR-B) — newest-first, minimal fields
    // (no detail, no age_s; consumer computes age from epoch_s if needed).
    // Distinct from reboot_reason above: this ring is NOT cleared-on-read
    // and captures every boot including untagged/hardware resets.
    bb_json_t hist = bb_json_arr_new();
    if (hist) {
        uint8_t n = s->reboot_history.count;
        if (n > BB_REBOOT_HISTORY_CAP) n = BB_REBOOT_HISTORY_CAP;
        for (uint8_t i = 0; i < n; i++) {
            // newest-first: walk backwards from the most recently pushed slot.
            uint8_t idx = (uint8_t)((s->reboot_history.head + (n - 1U - i)) % BB_REBOOT_HISTORY_CAP);
            const bb_reboot_hist_entry_t *e = &s->reboot_history.entries[idx];
            bb_json_t item = bb_json_obj_new();
            if (!item) break;
            bb_json_obj_set_string(item, "source", bb_reset_source_str((bb_reset_source_t)e->src));
            bb_json_obj_set_int(item, "epoch_s",  (int64_t)e->epoch_s);
            bb_json_obj_set_int(item, "uptime_s", (int64_t)e->uptime_s);
            bb_json_arr_append_obj(hist, item);
        }
        bb_json_obj_set_arr(obj, "reboot_history", hist);
    }
}
