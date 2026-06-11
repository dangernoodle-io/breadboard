#include "bb_ntp_info.h"
#include "bb_ntp.h"
#include "bb_info.h"
#include "bb_json.h"

/* JSON-Schema properties fragment contributed to the /api/info 200 schema. */
static const char k_ntp_schema_fragment[] =
    "\"ntp\":{\"type\":\"object\",\"properties\":{"
    "\"synced\":{\"type\":\"boolean\"},"
    "\"last_sync_unix\":{\"type\":\"number\"}}}";

static void ntp_info_extender(void *root)
{
    bb_json_t ntp = bb_json_obj_new();

    bb_json_obj_set_bool(ntp, "synced", bb_ntp_is_synced());
    bb_json_obj_set_number(ntp, "last_sync_unix",
                           (double)bb_ntp_last_sync_unix());

    bb_json_obj_set_obj((bb_json_t)root, "ntp", ntp);
}

void bb_ntp_register_info(void)
{
    bb_info_register_extender_ex(ntp_info_extender, k_ntp_schema_fragment);
}
