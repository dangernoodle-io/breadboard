#include "bb_partition.h"
#include <string.h>

// Deterministic mock modeling a 4MB-style layout.
// 5 partitions in order: nvs, otadata, ota_0 (running), ota_1 (next_ota), coredump.
static const bb_partition_info_t k_mock[] = {
    { "nvs",      "data", "nvs",      0x009000, 0x006000, false, false },
    { "otadata",  "data", "otadata",  0x00f000, 0x002000, false, false },
    { "ota_0",    "app",  "ota_0",    0x020000, 0x1b0000, true,  false },
    { "ota_1",    "app",  "ota_1",    0x1d0000, 0x1b0000, false, true  },
    { "coredump", "data", "coredump", 0x380000, 0x080000, false, false },
};

#define K_MOCK_COUNT (sizeof(k_mock) / sizeof(k_mock[0]))

bb_err_t bb_partition_list(bb_partition_info_t *out, size_t cap, size_t *count)
{
    if (!out || !count) return BB_ERR_INVALID_ARG;
    size_t n = K_MOCK_COUNT;
    for (size_t i = 0; i < cap && i < n; i++) {
        out[i] = k_mock[i];
    }
    *count = n;
    return BB_OK;
}

bb_err_t bb_partition_get_running(bb_partition_info_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    // ota_0 is the running partition (index 2)
    *out = k_mock[2];
    return BB_OK;
}

bb_err_t bb_partition_get_next_ota(bb_partition_info_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    // ota_1 is the next OTA partition (index 3)
    *out = k_mock[3];
    return BB_OK;
}
