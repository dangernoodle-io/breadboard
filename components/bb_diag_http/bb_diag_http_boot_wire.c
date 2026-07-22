// bb_diag_http_boot_wire — bb_diag_boot_render_envelope() (B1-1153, KB
// 1477): relocated verbatim from components/bb_diag/bb_diag_boot_wire.c,
// whose sole reason for depending on bb_http_server was this one function --
// bb_diag itself is bb_http_server-free after this split. Portable (compiles
// host + ESP-IDF; only bb_diag_boot_bind() -- still owned by bb_diag --
// needs to have already run), host-testable directly via
// bb_http_host_capture_begin/end (mirrors test_bb_http_json_obj_stream.c).
// See bb_diag_http.h for the prototype/doc this implements.

#include "bb_diag_http.h"

#include "bb_diag_boot_wire.h"
#include "bb_diag_event_priv.h"
#include "bb_clock.h"
#include "bb_data.h"
#include "bb_http_server.h"

#include <stddef.h>

// Worst case (8 reboot_history entries, every present-gated field populated)
// renders well under 1024 bytes -- proven today by
// test_wire_desc_diag_boot_render_history_eight_entries_wraparound
// (test_wire_desc_producers.c), which renders this exact descriptor into a
// 1024-byte buffer. Keep identical headroom here.
#define BB_DIAG_BOOT_RENDER_BUF_BYTES 1024

bb_err_t bb_diag_boot_render_envelope(bb_http_request_t *req)
{
    if (!req) return BB_ERR_INVALID_ARG;

    char   scratch[sizeof(bb_diag_boot_wire_t)];
    char   data_buf[BB_DIAG_BOOT_RENDER_BUF_BYTES];
    size_t data_len = 0;
    bb_data_render_req_t render_req = {
        .fmt         = BB_FORMAT_JSON,
        .key         = BB_DIAG_BOOT_TOPIC,
        .query       = NULL,
        .scratch     = scratch,
        .scratch_cap = sizeof(scratch),
        .buf         = data_buf,
        .buf_cap     = sizeof(data_buf),
        .out_len     = &data_len,
    };
    bb_err_t err = bb_data_render(&render_req);
    if (err != BB_OK) return err;

    bb_http_json_obj_stream_t obj;
    err = bb_http_resp_json_obj_begin(req, &obj);
    if (err != BB_OK) return err;

    bb_http_resp_json_obj_set_int(&obj, "ts_ms", (int64_t)bb_clock_now_ms64());
    bb_http_resp_json_obj_set_raw(&obj, "data", data_buf, data_len);

    return bb_http_resp_json_obj_end(&obj);
}
