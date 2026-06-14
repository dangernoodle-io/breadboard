#pragma once
// Host-only test hooks for bb_http_client. Tests register a canned response;
// subsequent bb_http_client_get, bb_http_client_get_stream, and
// bb_http_client_post calls return it.
// For _get_stream, the mock body is replayed in ~256-byte chunks.
#include <stddef.h>
#include <stdbool.h>
#include "bb_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Set the mock response. `body` is borrowed (typically a string literal);
// caller must keep it alive for the duration of any bb_http_client_* calls.
void bb_http_client_set_mock_response(const char *body, size_t body_len,
                                      int status_code);

// Force the next bb_http_client_get / bb_http_client_get_stream /
// bb_http_client_post call to return a transport error.
void bb_http_client_set_mock_transport_error(bb_err_t err);

// Reset to "no mock set" — subsequent calls return BB_ERR_INVALID_STATE.
// Also clears the last_post record.
void bb_http_client_clear_mock(void);

// -------------------------------------------------------------------------
// POST capture — populated by each bb_http_client_post call on the host.
// Strings point into the caller-supplied buffers; safe to read until the
// next call or clear_mock.  method is always "POST".
// -------------------------------------------------------------------------
typedef struct {
    bool        called;
    const char *method;        // "POST"
    const char *url;
    const char *body;          // pointer into caller's body arg (may be NULL)
    size_t      body_len;
    const char *content_type;  // effective content-type sent (never NULL after a call)
    bool        has_client_cert;  // true if cfg->client_cert_pem was non-NULL
    bool        has_client_key;   // true if cfg->client_key_pem was non-NULL
    bool        has_ca_cert;      // true if cfg->ca_cert_pem was non-NULL
} bb_http_client_post_record_t;

// Return the capture record from the last bb_http_client_post call.
// The record is reset by bb_http_client_clear_mock().
bb_http_client_post_record_t bb_http_client_get_last_post(void);

// -------------------------------------------------------------------------
// Session capture — populated by each bb_http_client_session_post call.
// Strings point into caller-supplied buffers; safe to read until the
// next session_post or clear_mock call.
// -------------------------------------------------------------------------
typedef struct {
    bool        called;
    const char *url;
    const char *body;          // pointer into caller's body arg (may be NULL)
    size_t      body_len;
    const char *content_type;  // effective content-type (never NULL after a call)
    int         canned_status; // status returned by session_post (set via hook)
} bb_http_client_session_record_t;

// Set the canned HTTP status code returned by session_post (default: 200).
// Also sets transport_result to BB_OK implicitly.
void bb_http_client_session_set_mock_status(int status_code);

// Force the next session_post call to return a transport error.
void bb_http_client_session_set_mock_transport_error(bb_err_t err);

// Return the capture record from the last bb_http_client_session_post call.
// Reset by bb_http_client_clear_mock().
bb_http_client_session_record_t bb_http_client_session_last_post(void);

// -------------------------------------------------------------------------
// Header capture — populated by bb_http_client_session_set_header calls.
// Stores a name→value map of all headers applied to the last opened session.
// Reset by bb_http_client_clear_mock() or when a new session is opened.
// -------------------------------------------------------------------------
#define BB_HTTP_CLIENT_HOST_MAX_HEADERS 16
#define BB_HTTP_CLIENT_HOST_HEADER_NAME_MAX  64
#define BB_HTTP_CLIENT_HOST_HEADER_VALUE_MAX 512

typedef struct {
    char name[BB_HTTP_CLIENT_HOST_HEADER_NAME_MAX];
    char value[BB_HTTP_CLIENT_HOST_HEADER_VALUE_MAX];
} bb_http_client_header_record_t;

// Return the number of headers applied via session_set_header since the last
// session_open or clear_mock.
int bb_http_client_session_header_count(void);

// Return a copy of the header at index i (0-based).  Returns a zeroed struct
// if i >= header_count.
bb_http_client_header_record_t bb_http_client_session_header_at(int i);

// Find a header by name (case-sensitive).  Returns a zeroed struct if not found.
bb_http_client_header_record_t bb_http_client_session_find_header(const char *name);

// Return the number of times bb_http_client_session_open has been called since
// the last bb_http_client_clear_mock().  Tests use this to detect re-opens
// after a session_close (e.g. after BB_SINK_HTTP_MAX_CONSEC_FAILURES).
int bb_http_client_session_open_count(void);

// Return the keep_alive value from the cfg passed to the last session_open call.
// Returns false if session_open has not been called or cfg was NULL.
bool bb_http_client_session_last_keep_alive(void);

#ifdef __cplusplus
}
#endif
