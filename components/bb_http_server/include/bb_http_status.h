#pragma once

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

#ifdef __cplusplus
}
#endif
