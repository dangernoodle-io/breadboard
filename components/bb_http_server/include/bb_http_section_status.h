#pragma once
// bb_http_section_status -- pure bb_err_t/bb_http_section_apply_result_t ->
// HTTP status mappers for the bb_http_section dispatcher (bb_http_section
// PR). No FreeRTOS/ESP-IDF/httpd types -- host-testable in isolation,
// mirroring cache_route_status.h and bb_wifi_http_apply_status.h.
//
// The ESP-IDF-only dispatcher (platform/espidf/bb_http_server/
// bb_http_section_dispatch.c) calls these and delegates the resulting
// bb_err_t -> HTTP-status decision here, so Coveralls sees and the host test
// suite exercises every branch of the mapping even though the caller's own
// inputs (a live namespace + httpd_req_t) cannot be host-compiled. This is
// the ONE copy of the mapping -- production and the host tests both call it.

#include "bb_http_section.h"

#ifdef __cplusplus
extern "C" {
#endif

// GET path: rc is whatever a namespace's render() hook itself returned.
//   BB_OK             -> 200
//   BB_ERR_NOT_FOUND   -> 404 (a render hook's own "no such name" reject)
//   any other non-BB_OK -> 500 (render is a live-state read, not
//                          caller-input validation -- any other failure is
//                          an internal error)
int bb_http_section_status_for_render(bb_err_t rc);

// PATCH/POST path: `result` carries a stage tag (see
// bb_http_section_apply_result_t) alongside the bb_err_t, so this mapping is
// correct BY CONSTRUCTION rather than guessed from the error code alone --
// see bb_data_parse()/bb_data_commit() (components/bb_data) for the split
// this depends on.
//
// BB_HTTP_SECTION_STAGE_PARSE (the wire body itself couldn't be
// decoded/resolved):
//   BB_ERR_PARSE_GRAMMAR / BB_ERR_PARSE_INCOMPLETE -> 400 (malformed/
//                                                      truncated body)
//   BB_ERR_NOT_FOUND                                -> 404 (unresolvable name)
//   BB_ERR_UNSUPPORTED                              -> 405 (no apply hook /
//                                                      unsupported wire format)
//   any other non-BB_OK                             -> 500 (internal error --
//                                                      resolving/decoding a
//                                                      request should never
//                                                      fail any other way)
//
// BB_HTTP_SECTION_STAGE_COMMIT (decode succeeded; a downstream step
// rejected or failed -- now safe to trust, since a commit-stage result can
// ONLY arise after a successful decode):
//   BB_OK               -> 200
//   BB_ERR_VALIDATION    -> 400 (domain-level reject of otherwise
//                           well-formed input)
//   BB_ERR_UNSUPPORTED   -> `unsupported_status_override` if non-zero,
//                           else 405 (this namespace doesn't support the
//                           write) -- see bb_http_section_ns_t's own
//                           BB_ERR_UNSUPPORTED-is-overloaded doc
//                           (bb_http_section.h) for why a single hardcoded
//                           405 can't serve every namespace.
//   any other non-BB_OK  -> 500 (a genuine downstream failure, e.g.
//                           BB_ERR_INVALID_STATE)
//
// `unsupported_status_override` is the calling namespace's own
// bb_http_section_ns_t.unsupported_status (0 = "use the 405 default").
int bb_http_section_status_for_apply(bb_http_section_apply_result_t result,
                                      int unsupported_status_override);

#ifdef __cplusplus
}
#endif
