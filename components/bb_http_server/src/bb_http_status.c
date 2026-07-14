#include "bb_http_status.h"
#include "bb_log.h"
#include <stddef.h>
#include <stdio.h>

static const char *TAG = "bb_http_status";

const char *bb_http_status_reason(int status_code)
{
    switch (status_code) {
        case 200: return "200 OK";
        case 201: return "201 Created";
        case 202: return "202 Accepted";
        case 204: return "204 No Content";
        case 302: return "302 Found";
        case 400: return "400 Bad Request";
        case 401: return "401 Unauthorized";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 408: return "408 Request Timeout";
        case 409: return "409 Conflict";
        case 412: return "412 Precondition Failed";
        case 413: return "413 Payload Too Large";
        case 422: return "422 Unprocessable Entity";
        case 500: return "500 Internal Server Error";
        case 501: return "501 Not Implemented";
        case 503: return "503 Service Unavailable";
        default:  return NULL;
    }
}

const char *bb_http_status_line(int status_code, char *fallback_buf, size_t fallback_buf_len)
{
    const char *known = bb_http_status_reason(status_code);
    if (known) return known;

    // A code outside the valid HTTP status range is not a status at all —
    // reject rather than format it. Guards against snprintf silently
    // truncating a pathological value (e.g. INT_MIN) into a value that LOOKS
    // like a valid formatted line but reports a bogus/truncated number to
    // the client while still returning non-NULL as if it had succeeded.
    if (status_code < 100 || status_code > 599) {
        bb_log_w(TAG, "rejecting invalid HTTP status code %d (must be 100-599)", status_code);
        return NULL;
    }

    // Untabled code (B1-954): rather than leaving the response status unset
    // (the historical fail-silent behaviour that shipped a wrong status to
    // the client for both 413 and, previously, 501), emit a numerically
    // correct line with a generic reason phrase. The number is what an HTTP
    // client acts on; the phrase is cosmetic. Loud, not silent: warn so a
    // missing table entry gets caught instead of quietly tolerated forever.
    bb_log_w(TAG, "untabled HTTP status code %d — add it to bb_http_status.c", status_code);
    if (!fallback_buf || fallback_buf_len == 0) return NULL;
    // status_code is now bounded to [100, 599] (3 digits), so
    // "%d Unknown Status" is at most 19 chars + NUL — always fits the 24-byte
    // buffer every call site passes; snprintf cannot fail/truncate here.
    snprintf(fallback_buf, fallback_buf_len, "%d Unknown Status", status_code);
    return fallback_buf;
}
