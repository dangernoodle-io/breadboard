#include "bb_route_match.h"

#include <string.h>

// See bb_route_match.h for the full contract, the INTERIM-subset caveat, and
// the target platform-hook shape. Two branches only: a trailing-'*' pattern
// is a bounded prefix match; anything else is a bounded exact match.
bool bb_route_uri_match(const char *pattern, const char *uri, size_t match_upto)
{
    if (!pattern || !uri) return false;

    size_t plen = strlen(pattern);

    if (plen > 0 && pattern[plen - 1] == '*') {
        size_t prefix_len = plen - 1;
        if (match_upto < prefix_len) return false;
        return memcmp(uri, pattern, prefix_len) == 0;
    }

    if (plen != match_upto) return false;
    return memcmp(uri, pattern, match_upto) == 0;
}
