// bb_mdns_cache — pure config-validation predicate (the test seam for
// bb_mdns_cache_start()'s guard checks). Compiled on host and ESP-IDF; no
// platform types, no locks, no I/O.

#include "bb_mdns_cache.h"

bb_err_t bb_mdns_cache_validate_config(size_t entry_size,
                                       const bb_mdns_txt_field_t *txt_fields,
                                       size_t txt_count, size_t entry_max)
{
    // A descriptor implies a consumer struct -- entry_size == 0 with
    // txt_fields set is a config error, not a silent identity-only fallback.
    if (txt_fields && txt_count > 0 && entry_size == 0) return BB_ERR_INVALID_ARG;

    size_t effective = entry_size ? entry_size : sizeof(bb_mdns_cache_entry_t);
    if (effective > entry_max) return BB_ERR_INVALID_ARG;

    return BB_OK;
}
