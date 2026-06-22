#include "bb_http_query.h"
#include <string.h>

bool bb_http_query_token_present(const char *query, const char *key)
{
    if (!query || !key) return false;
    size_t klen = strlen(key);
    const char *p = query;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg >= klen && strncmp(p, key, klen) == 0 &&
            (seg == klen || p[klen] == '=')) {
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}
