// bb_mdns_cache — pure key-format + result-validity logic (the test seam).
// Compiled on host and ESP-IDF; no platform types, no locks, no I/O. Called
// verbatim by the ESP-IDF glue (hello/bye handlers + re-query worker) and by
// host tests.

#include "bb_mdns_cache.h"

#include <string.h>
#include <stdio.h>

bb_err_t bb_mdns_cache_build_key(const char *prefix, const char *instance_name,
                                 char *out, size_t out_size)
{
    if (!out) return BB_ERR_INVALID_ARG;
    if (out_size == 0) return BB_ERR_INVALID_ARG;
    if (!instance_name) return BB_ERR_INVALID_ARG;
    if (instance_name[0] == '\0') return BB_ERR_INVALID_ARG;

    const char *p = prefix;
    if (!p) p = BB_MDNS_CACHE_KEY_PREFIX_DEFAULT;
    if (p[0] == '\0') p = BB_MDNS_CACHE_KEY_PREFIX_DEFAULT;

    int n = snprintf(out, out_size, "%s%s", p, instance_name);
    if ((size_t)n >= out_size) {
        // snprintf still NUL-terminates the truncated result within out_size.
        return BB_ERR_NO_SPACE;
    }
    return BB_OK;
}

bool bb_mdns_cache_result_valid(const char *instance_name, const char *ip4)
{
    if (!instance_name) return false;
    if (instance_name[0] == '\0') return false;
    if (!ip4) return false;
    if (ip4[0] == '\0') return false;

    for (const char *c = ip4; *c != '\0'; c++) {
        if (*c != '.' && (*c < '0' || *c > '9')) return false;
    }
    return true;
}
