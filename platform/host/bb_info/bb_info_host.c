#include "bb_info.h"

#define BB_INFO_MAX_EXTENDERS 4
#define BB_HEALTH_MAX_EXTENDERS 4

static bb_info_extender_fn s_extenders[BB_INFO_MAX_EXTENDERS];
static int s_extender_count = 0;
static bool s_frozen = false;

static bb_info_extender_fn s_health_extenders[BB_HEALTH_MAX_EXTENDERS];
static int s_health_extender_count = 0;

bb_err_t bb_info_register_extender(bb_info_extender_fn fn)
{
    if (!fn) return BB_ERR_INVALID_ARG;
    if (s_frozen) return BB_ERR_INVALID_STATE;
    if (s_extender_count >= BB_INFO_MAX_EXTENDERS) return BB_ERR_NO_SPACE;
    s_extenders[s_extender_count++] = fn;
    return BB_OK;
}

bb_err_t bb_health_register_extender(bb_info_extender_fn fn)
{
    if (!fn) return BB_ERR_INVALID_ARG;
    if (s_frozen) return BB_ERR_INVALID_STATE;
    if (s_health_extender_count >= BB_HEALTH_MAX_EXTENDERS) return BB_ERR_NO_SPACE;
    s_health_extenders[s_health_extender_count++] = fn;
    return BB_OK;
}
