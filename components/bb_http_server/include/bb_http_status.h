#pragma once

#include <stddef.h>

// Single source of truth for HTTP status-code → reason-phrase mapping, shared
// by the ESP-IDF and host bb_http backends so a new code is added in exactly
// one place (previously the espidf switch and the host mirror switch had to be
// kept in sync by hand — the 422 case shipped to one but not the other).

#ifdef __cplusplus
extern "C" {
#endif

// Return the full HTTP status line ("200 OK", "422 Unprocessable Entity", ...)
// for a known status code, or NULL if the code is not recognised. The returned
// pointer is a static string literal (safe to hand to httpd_resp_set_status,
// which stores the pointer without copying).
const char *bb_http_status_reason(int status_code);

// Return a status line for ANY status code (B1-954): a known code returns the
// same static literal as bb_http_status_reason(); an untabled code falls back
// to a numerically-correct, generically-phrased line ("<code> Unknown
// Status") formatted into fallback_buf, and logs a warning so the missing
// table entry gets noticed instead of silently shipping the wrong status.
// Only the numeric code is meaningful to an HTTP client — the reason phrase
// is cosmetic — so this can never regress client-visible behaviour the way a
// no-op'd/unset status can. fallback_buf must outlive the call site's use of
// the returned pointer (e.g. until httpd_resp_send*() is called); callers on
// backends where the string pointer must persist should pass a
// file-scope/static buffer, not a stack buffer. Returns NULL only if
// fallback_buf is NULL/zero-length and the code is untabled.
const char *bb_http_status_line(int status_code, char *fallback_buf, size_t fallback_buf_len);

#ifdef __cplusplus
}
#endif
