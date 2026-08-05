#pragma once
// Host-only capture-stub backend for bb_data_http. Installs a send_fn that
// records every sent frame into a fixed-size ring so host tests can assert
// on what was actually flushed to each client, without a real socket/httpd.
// Tests are free to install their own render_fn/generation_fn directly
// (bb_data_http_set_render_fn / bb_data_http_set_generation_fn) -- this
// stub only owns the send-side capture, mirroring the real (later) espidf
// backend's send_fn role.
#include "bb_core.h"
#include "bb_data_http.h"  // bb_data_http_client_t (opaque)

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Installs the capture send_fn via bb_data_http_set_send_fn(). Call once per
// test (or after bb_data_http_reset_for_test()) before exercising sends.
void bb_data_http_host_install_send(void);

// Installs a capture abort_fn via bb_data_http_set_abort_fn() -- records the
// aborted client (bb_data_http_host_last_aborted_client()) AND releases it,
// exercising the same "abort_fn IS installed" path a real backend takes
// (see bb_data_http_abort_fn's doc, bb_data_http.h). Optional: a test that
// wants to exercise the core's own no-abort-fn fallback
// (bb_data_http_client_release() called directly) should simply not call
// this. Call once per test (or after bb_data_http_reset_for_test()).
void bb_data_http_host_install_abort(void);

// Returns the client last passed to the installed abort_fn (see
// bb_data_http_host_install_abort()), or NULL if none has fired since the
// last bb_data_http_host_reset().
bb_data_http_client_t *bb_data_http_host_last_aborted_client(void);

// B1-1424: makes the next `count` calls to the installed send_fn return
// `rc` (which must not be BB_OK -- this is a failure-injection knob, not a
// general override) WITHOUT capturing a frame for any of them, so host
// tests can exercise bb_data_http_sweep_step()'s flush-contract retriable
// (rc == BB_ERR_TIMEOUT) and fatal (any other non-BB_OK rc) paths. Each
// failing call decrements the remaining count by one; once it reaches 0,
// send_fn reverts to its normal capture-and-succeed behavior. count=0 is a
// no-op (nothing gets forced to fail).
void bb_data_http_host_fail_next(bb_err_t rc, size_t count);

// Clears the captured-frame ring. Also clears any pending fail-injection set
// by bb_data_http_host_fail_next() and the last-aborted-client record --
// does NOT touch the installed send_fn/abort_fn -- call
// bb_data_http_host_install_send()/bb_data_http_host_install_abort() again
// after bb_data_http_reset_for_test() to reinstall them.
void bb_data_http_host_reset(void);

// Number of frames captured since the last bb_data_http_host_reset().
size_t bb_data_http_host_frame_count(void);

// Copies capture record `idx` (0 == oldest). `buf`/`buf_cap` receive up to
// buf_cap bytes of the captured frame; out_len receives the frame's actual
// length (may exceed buf_cap -- only min(out_len, buf_cap) bytes are
// copied). Any output pointer may be NULL to skip that field.
// Returns BB_ERR_NOT_FOUND if idx >= bb_data_http_host_frame_count().
bb_err_t bb_data_http_host_frame_at(size_t idx, int *out_fd, bool *out_is_ws,
                                    char *buf, size_t buf_cap, size_t *out_len);

// Copies capture record `idx`'s resolved send_fn `key` (B1-1123 PR-2) into
// `buf`/`buf_cap`, truncating and NUL-terminating if `key` doesn't fit.
// `buf` may be NULL/`buf_cap` may be 0 to just check idx validity.
// Returns BB_ERR_NOT_FOUND if idx >= bb_data_http_host_frame_count().
bb_err_t bb_data_http_host_frame_key_at(size_t idx, char *buf, size_t buf_cap);

#ifdef __cplusplus
}
#endif
