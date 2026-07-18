#pragma once
// Host-only capture-stub backend for bb_data_http. Installs a send_fn that
// records every sent frame into a fixed-size ring so host tests can assert
// on what was actually flushed to each client, without a real socket/httpd.
// Tests are free to install their own render_fn/generation_fn directly
// (bb_data_http_set_render_fn / bb_data_http_set_generation_fn) -- this
// stub only owns the send-side capture, mirroring the real (later) espidf
// backend's send_fn role.
#include "bb_core.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Installs the capture send_fn via bb_data_http_set_send_fn(). Call once per
// test (or after bb_data_http_reset_for_test()) before exercising sends.
void bb_data_http_host_install_send(void);

// Clears the captured-frame ring. Does NOT touch the installed send_fn --
// call bb_data_http_host_install_send() again after
// bb_data_http_reset_for_test() to reinstall it.
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

#ifdef __cplusplus
}
#endif
