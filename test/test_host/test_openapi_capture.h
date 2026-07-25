#pragma once

// test_openapi_capture -- shared helper that drives bb_openapi_emit_stream()
// through the bb_http_host capture seam and parses the captured body with
// cJSON exactly once, so stream-path assertions (PR 3-5 of B1-1054) can
// compare parsed JSON instead of each hand-rolling the
// capture_begin/emit_stream/capture_end/cJSON_Parse dance (already repeated
// verbatim across test_openapi_emit.c and test_sse_schema_fidelity.c -- this
// is roughly the tenth instance, past the project's second-instance
// extraction threshold).

#include "bb_openapi.h"
#include "bb_http_host.h"

#include <cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

// Result of driving bb_openapi_emit_stream() and parsing its output.
//
// `status` is bb_openapi_emit_stream()'s own return value, always populated
// (never skipped, even on early return) so a caller can assert it exactly as
// the ad hoc callers this helper replaces do today
// (TEST_ASSERT_EQUAL(BB_OK, bb_openapi_emit_stream(...))).
//
// Three distinct failure shapes are each visible separately:
//   1. status != BB_OK                         -- the emitter itself failed.
//   2. status == BB_OK, cap.body == NULL/empty -- emitter ok, nothing captured.
//   3. status == BB_OK, cap.body non-empty,
//      doc == NULL                             -- emitter ok, captured body
//                                                  is not valid JSON (e.g. a
//                                                  malformed schema literal
//                                                  spliced verbatim) -- a real
//                                                  regression shape, distinct
//                                                  from a deliberately forced
//                                                  failure.
//
// On success: status == BB_OK, cap.body is the raw JSON text (non-NULL), doc
// is the parsed tree (non-NULL).
typedef struct {
    bb_http_host_capture_t cap;
    cJSON                 *doc;
    bb_err_t               status;
} test_openapi_capture_result_t;

// Drive bb_openapi_emit_stream(&meta) through a fresh bb_http_host capture
// slot and parse the resulting body with cJSON.
//
// Single-slot invariant: only one capture may be live at a time. Calling this
// again before the previous result is freed (test_openapi_capture_free() or
// test_openapi_capture_teardown()) fails the current test loudly via
// TEST_FAIL_MESSAGE rather than silently overwriting the still-live result
// and orphaning its doc/cap.body.
//
// result.doc == NULL if:
//   - result.status (the emitter's own return) is not BB_OK, or
//   - the capture produced no body (cap.body == NULL or empty), or
//   - the body failed to parse as JSON (cJSON_Parse returned NULL) --
//     result.status is still BB_OK in this last case, since the emitter
//     itself succeeded; only the captured output was malformed.
// In every NULL-doc case, cap is still populated from the capture (body may
// be NULL) and must still be freed by the caller.
test_openapi_capture_result_t test_openapi_capture(const bb_openapi_meta_t *meta);

// Release both the parsed cJSON doc (if any) and the underlying capture
// buffer. Safe to call on a partially-initialised or all-zero result, and
// safe to call twice (idempotent).
void test_openapi_capture_free(test_openapi_capture_result_t *result);

// tearDown-driven safety net: test_openapi_capture() records the most
// recently produced result's doc/body pointers internally. If a
// TEST_ASSERT between test_openapi_capture() and test_openapi_capture_free()
// fails, Unity longjmps out of the test and the explicit free never runs --
// leaking result.doc + result.cap.body for that one test run (pre-existing
// exposure; the old tree-emitter bb_json_free(doc) call sites had the same
// gap). Call this from the shared tearDown() (test_main.c) so a failed
// assertion can never leak past the test that caused it. Safe to call when
// no capture is live (no-op), and safe alongside an explicit
// test_openapi_capture_free() call in the test body -- that call already
// clears the internally tracked pointers, so this becomes a no-op too (no
// double free).
void test_openapi_capture_teardown(void);

#ifdef __cplusplus
}
#endif
