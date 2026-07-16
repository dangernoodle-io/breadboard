#include "bb_system.h"

bool bb_system_boot_fail_count_over(uint8_t count, uint8_t threshold)
{
    return count >= threshold;
}

bool bb_system_boot_fail_over_threshold(void)
{
    return bb_system_boot_fail_count_over(bb_system_boot_count_get(), BB_SYSTEM_BOOT_FAIL_THRESHOLD);
}
