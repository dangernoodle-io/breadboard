#pragma once

#include <stdbool.h>

// Query-string helpers shared by the ESP-IDF and host bb_http backends.

#ifdef __cplusplus
extern "C" {
#endif

// Return true if `query` (an '&'-separated query string, e.g.
// "a=1&flag&b=2") contains `key` as a whole token — either bare ("flag") or
// with a value ("key=..."). A leading prefix match that is not a whole token
// (e.g. key "fl" against segment "flag") does not count. NULL query/key → false.
bool bb_http_query_token_present(const char *query, const char *key);

#ifdef __cplusplus
}
#endif
