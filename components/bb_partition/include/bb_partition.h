#pragma once
#include "bb_core.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     label[17];     // partition label (16 + NUL)
    char     type[8];       // "app" | "data" | "unknown"
    char     subtype[16];   // "factory","ota_0","ota_1","nvs","otadata","coredump","phy",...
    uint32_t offset;        // flash offset (address)
    uint32_t size;          // bytes
    bool     running;       // == esp_ota_get_running_partition()
    bool     next_ota;      // == esp_ota_get_next_update_partition(NULL)
} bb_partition_info_t;

// Fill out[0..cap) with all partitions; *count = total found (may exceed cap).
bb_err_t bb_partition_list(bb_partition_info_t *out, size_t cap, size_t *count);
// Fill *out with the running partition; BB_ERR_NOT_FOUND if none.
bb_err_t bb_partition_get_running(bb_partition_info_t *out);
// Fill *out with the next-OTA-update partition; BB_ERR_NOT_FOUND if none.
bb_err_t bb_partition_get_next_ota(bb_partition_info_t *out);

// The legacy GET /api/diag/partitions exact route (bb_partition_routes_init)
// is DELETED (B1-1077 PR-3a) -- no external consumer was found (repo-wide +
// workspace-wide grep for the literal path), and it duplicated the shipped
// "storage/partitions" bb_diag section (bb_diag_storage_partitions_register(),
// components/bb_diag/bb_diag_storage_partitions.c) one-for-one. That section
// now carries a bbtool:init marker of its own (previously floor-handwire-only)
// so both floor and codegen (smoke) compositions keep partition-table
// reachability after this deletion.

#ifdef __cplusplus
}
#endif
