// Pure (host-testable) bb_cache serializer for bb_display's health.display
// retained event topic. No FreeRTOS or ESP-IDF types here.
#include "bb_display_info_event_priv.h"

void bb_display_serialize(bb_json_t obj, const void *snap)
{
    const bb_display_snap_t *s = snap;
    bb_json_obj_set_bool(obj, "present", s->present);
    if (s->present) {
        bb_json_obj_set_string(obj, "panel",   s->panel);
        bb_json_obj_set_number(obj, "width",   (double)s->width);
        bb_json_obj_set_number(obj, "height",  (double)s->height);
        bb_json_obj_set_bool  (obj, "enabled", s->enabled);
    }
}
