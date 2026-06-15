#include "bb_ntp_info.h"
#include "bb_ntp.h"
#include "bb_info.h"
#include "bb_json.h"

/* JSON-Schema value for the "ntp" section. */
static const char k_ntp_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"synced\":{\"type\":\"boolean\"},"
    "\"last_sync_unix\":{\"type\":\"number\"}}}";

static void ntp_section_get(bb_json_t section, void *ctx)
{
    (void)ctx;
    bb_json_obj_set_bool(section, "synced", bb_ntp_is_synced());
    bb_json_obj_set_number(section, "last_sync_unix",
                           (double)bb_ntp_last_sync_unix());
}

void bb_ntp_register_info(void)
{
    bb_info_register_section("ntp", ntp_section_get, NULL, k_ntp_schema);
}
