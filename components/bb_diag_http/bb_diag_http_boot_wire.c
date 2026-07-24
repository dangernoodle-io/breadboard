// bb_diag_http_boot_wire — bb_diag_boot_render_envelope() (B1-1153, KB
// 1477): relocated verbatim from components/bb_diag/bb_diag_boot_wire.c,
// whose sole reason for depending on bb_http_server was this one function --
// bb_diag itself is bb_http_server-free after this split. Portable (compiles
// host + ESP-IDF; only bb_diag_boot_bind() -- still owned by bb_diag --
// needs to have already run), host-testable directly via
// bb_http_host_capture_begin/end (mirrors test_bb_http_json_obj_stream.c).
// See bb_diag_http.h for the prototype/doc this implements.

#include "bb_diag_http.h"

#include "bb_diag_http_boot_wire_priv.h"

#include "bb_diag_boot_wire.h"
#include "bb_diag_event_priv.h"
#include "bb_clock.h"
#include "bb_data.h"
#include "bb_http_server.h"

#include <stddef.h>
#include <string.h>

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

// ---------------------------------------------------------------------------
// GET /api/diag/boot RESPONSE schema (B1-1059 emit boot, B1-1189 design).
// DISTINCT from components/bb_diag/bb_diag_boot_wire.c's "diag.boot" SSE
// topic-schema machinery (bb_diag_boot_get_schema()/
// bb_diag_boot_ensure_schema_patched()/s_diag_boot_schema_buf/
// k_diag_boot_schema) -- same desc/meta pair, entirely separate composer/
// buffer/symbols, serving the REST route's {"ts_ms","data"} envelope shape
// instead of the bb_data topic's bare-object shape. Config OFF (default)
// keeps BB_DIAG_BOOT_GET_SCHEMA_LITERAL byte-identical -- config ON composes
// it at init from bb_diag_boot_wire_desc/bb_diag_boot_wire_meta via a local
// envelope composer plugged into bb_serialize_meta_ensure_composed() -- see
// bb_diag_http_boot_wire_ensure_schema_patched() below. Accepted config-ON
// delta (see test_bb_diag_boot_wire_envelope_meta_golden.c): the composed
// "data" body gains a trailing "additionalProperties":false at each object
// depth (the engine always closes every rendered object), and
// "reboot_history.items" gets no "required" list (engine limitation, not a
// content mismatch); no outer additionalProperties:false; config-OFF ships
// the untouched literal.
// ---------------------------------------------------------------------------
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)

// Envelope composer: wraps bb_serialize_meta_openapi_schema()'s composed
// "diag_boot" object schema in the fixed {"ts_ms":<int>,"data":<...>}
// envelope -- matches bb_serialize_meta_composer_fn's signature so it can be
// plugged directly into bb_serialize_meta_ensure_composed() (the SIMPLE
// path: no open_fragment/root_close -- this envelope is a single
// self-contained nested object wrapped by one fixed key pair, not a
// composite root with sibling sections spliced in).
static bb_err_t diag_http_boot_wire_envelope_composer(const bb_serialize_desc_t      *desc,
                                                        const bb_serialize_desc_meta_t *meta,
                                                        char *out, size_t out_size, size_t *out_len)
{
    static const char k_prefix[] =
        "{\"type\":\"object\",\"properties\":{\"ts_ms\":{\"type\":\"integer\"},\"data\":";
    static const char k_suffix[] = "},\"required\":[\"ts_ms\",\"data\"]}";
    size_t plen = sizeof(k_prefix) - 1;
    size_t slen = sizeof(k_suffix) - 1;

    if (out_size < plen) {
        out[0] = '\0';
        return BB_ERR_NO_SPACE;
    }
    memcpy(out, k_prefix, plen);

    size_t data_len = 0;
    bb_err_t rc = bb_serialize_meta_openapi_schema(desc, meta, out + plen, out_size - plen, &data_len);
    if (rc != BB_OK) {
        out[0] = '\0';
        return rc;
    }

    if (plen + data_len + slen + 1 > out_size) {
        out[0] = '\0';
        return BB_ERR_NO_SPACE;
    }
    memcpy(out + plen + data_len, k_suffix, slen + 1);  // incl NUL

    *out_len = plen + data_len + slen;
    return BB_OK;
}

#ifdef BB_SERIALIZE_META_TESTING
// Test-only cap override (mirrors bb_health_schema.c's
// bb_health_schema_set_root_close_cap_for_test() precedent) -- shrinks the
// effective capacity passed to the composer so a host test can force THIS
// composer's own bounds-check branches (prefix-too-small / suffix-too-small)
// independent of the shared BB_SERIALIZE_META_TESTING force_no_space seam
// (which only fails INSIDE bb_serialize_meta_openapi_schema() itself, not
// this composer's own prefix/suffix arithmetic). 0 = use the real
// sizeof(s_diag_http_boot_envelope_schema_buf). Never widens beyond the real
// buffer -- a test may only SHRINK the effective cap to force a genuine
// overflow, never grow past the buffer's actual static allocation.
static size_t s_test_envelope_buf_cap = 0;
void bb_diag_http_boot_wire_set_test_envelope_buf_cap(size_t cap) { s_test_envelope_buf_cap = cap; }
#endif /* BB_SERIALIZE_META_TESTING */

// Sized with headroom over the golden-proven composed body
// (test_bb_diag_boot_wire_envelope_meta_golden.c's k_expected_envelope_schema
// is 1013 bytes + NUL) -- any future desync trips the composer's own
// bounded-buffer BB_ERR_NO_SPACE contract instead of silently truncating.
static char s_diag_http_boot_envelope_schema_buf[1280];
bb_err_t bb_diag_http_boot_wire_ensure_schema_patched(void)
{
    size_t cap = sizeof(s_diag_http_boot_envelope_schema_buf);
#ifdef BB_SERIALIZE_META_TESTING
    if (s_test_envelope_buf_cap && s_test_envelope_buf_cap < cap) cap = s_test_envelope_buf_cap;
#endif
    return bb_serialize_meta_ensure_composed(diag_http_boot_wire_envelope_composer,
                                              &bb_diag_boot_wire_desc, &bb_diag_boot_wire_meta,
                                              s_diag_http_boot_envelope_schema_buf, cap);
}

#else
static const char k_diag_http_boot_envelope_schema[] = BB_DIAG_BOOT_GET_SCHEMA_LITERAL;
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */

const char *bb_diag_http_boot_wire_get_schema(void)
{
#if defined(CONFIG_BB_OPENAPI_RUNTIME_META)
    return s_diag_http_boot_envelope_schema_buf;
#else
    return k_diag_http_boot_envelope_schema;
#endif /* CONFIG_BB_OPENAPI_RUNTIME_META */
}
