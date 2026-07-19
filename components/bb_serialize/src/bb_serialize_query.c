// Request-scoped query-param carrier -- see bb_serialize.h for the seam
// contract. Plain lookup helper, no heap, no I/O.
#include "bb_serialize.h"

#include <string.h>

const char *bb_serialize_query_get(const bb_serialize_query_t *q, const char *key)
{
    if (!q || !key) return NULL;

    // Clamp against the fixed params[] capacity -- q->count is caller-
    // supplied (e.g. a future HTTP query-string parser feeding untrusted
    // input) and must never be trusted past BB_SERIALIZE_QUERY_MAX_PARAMS,
    // or an inflated count drives an out-of-bounds read.
    size_t n = q->count < BB_SERIALIZE_QUERY_MAX_PARAMS ? q->count : BB_SERIALIZE_QUERY_MAX_PARAMS;
    for (size_t i = 0; i < n; i++) {
        if (q->params[i].key && strcmp(q->params[i].key, key) == 0) {
            return q->params[i].value;
        }
    }
    return NULL;
}
