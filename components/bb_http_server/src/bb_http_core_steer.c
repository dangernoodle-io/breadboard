#include "bb_http_core_steer.h"
#include "bb_wdt.h"

int32_t bb_http_core_steer_resolve(int configured_core, uint32_t claimed_mask,
                                    int num_cores, int32_t no_affinity)
{
    int steered = bb_wdt_steer_core(configured_core, claimed_mask, num_cores);
    return steered == BB_WDT_CORE_ANY ? no_affinity : (int32_t)steered;
}
