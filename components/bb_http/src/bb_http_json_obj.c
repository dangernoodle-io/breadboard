#include "bb_http.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

// ============================================================================
// STREAMING JSON OBJECT API — portable (uses only bb_http_resp_send_chunk)
// ============================================================================

// Maximum nesting depth (top-level object counts as depth 0; first nested
// object/array is depth 1). Matches the _needs_comma[] array size in the
// stream struct declared in bb_http.h.
#define BB_JSON_OBJ_MAX_DEPTH 8

// Flush the internal buffer to the network via chunked transfer. Called
// internally when the buffer is full or at _end(). Returns sticky error
// if already poisoned.
static bb_err_t obj_flush(bb_http_json_obj_stream_t *s)
{
    if (s->_err != BB_OK) return s->_err;
    if (s->_buf_len == 0)  return BB_OK;

    bb_http_request_t *req = (bb_http_request_t *)s->_req;
    bb_err_t err = bb_http_resp_send_chunk(req, s->_buf, (int)s->_buf_len);
    s->_buf_len = 0;
    if (err != BB_OK) s->_err = err;
    return err;
}

// Append bytes to the internal buffer, flushing first if they would overflow.
static bb_err_t obj_append(bb_http_json_obj_stream_t *s,
                           const char *data, size_t len)
{
    if (s->_err != BB_OK) return s->_err;
    if (len == 0)          return BB_OK;

    // Flush first if the incoming data won't fit
    if (s->_buf_len + len > BB_HTTP_JSON_OBJ_BUF_SIZE) {
        bb_err_t err = obj_flush(s);
        if (err != BB_OK) return err;
    }

    // If it still won't fit (single token larger than the buffer), send
    // it directly rather than overflowing.
    if (len > BB_HTTP_JSON_OBJ_BUF_SIZE) {
        bb_http_request_t *req = (bb_http_request_t *)s->_req;
        bb_err_t err = bb_http_resp_send_chunk(req, data, (int)len);
        if (err != BB_OK) s->_err = err;
        return err;
    }

    memcpy(s->_buf + s->_buf_len, data, len);
    s->_buf_len += len;
    return BB_OK;
}

// Emit a single character via the buffer.
static inline bb_err_t obj_putc(bb_http_json_obj_stream_t *s, char c)
{
    return obj_append(s, &c, 1);
}

// Emit a comma if one is needed at the current depth, then mark that the
// next field will need a comma.
static bb_err_t obj_maybe_comma(bb_http_json_obj_stream_t *s)
{
    if (s->_err != BB_OK) return s->_err;
    uint8_t d = s->_depth;
    if (d >= BB_JSON_OBJ_MAX_DEPTH) {
        s->_err = BB_ERR_INVALID_ARG;
        return s->_err;
    }
    if (s->_needs_comma[d]) {
        bb_err_t err = obj_putc(s, ',');
        if (err != BB_OK) return err;
    }
    s->_needs_comma[d] = 1;
    return BB_OK;
}

// Emit a JSON-escaped string value (without the surrounding quotes).
static bb_err_t obj_emit_str_escaped(bb_http_json_obj_stream_t *s,
                                     const char *str)
{
    if (!str) {
        return obj_append(s, "null", 4);
    }
    bb_err_t err = obj_putc(s, '"');
    if (err != BB_OK) return err;

    for (const char *p = str; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') {
            err = obj_append(s, "\\\"", 2);
        } else if (c == '\\') {
            err = obj_append(s, "\\\\", 2);
        } else if (c == '\n') {
            err = obj_append(s, "\\n", 2);
        } else if (c == '\r') {
            err = obj_append(s, "\\r", 2);
        } else if (c == '\t') {
            err = obj_append(s, "\\t", 2);
        } else if (c < 0x20) {
            // Control characters — emit as \uXXXX
            char esc[7];
            int n = snprintf(esc, sizeof(esc), "\\u%04x", c);
            err = obj_append(s, esc, (size_t)n);
        } else {
            err = obj_putc(s, (char)c);
        }
        if (err != BB_OK) return err;
    }

    return obj_putc(s, '"');
}

// Emit "key": (the key part of a key/value pair).
// When key is NULL we are inside an array context — emit only the comma,
// not a key/colon pair.
static bb_err_t obj_emit_key(bb_http_json_obj_stream_t *s, const char *key)
{
    bb_err_t err = obj_maybe_comma(s);
    if (err != BB_OK) return err;
    if (!key) return BB_OK;  // array element: no key
    err = obj_emit_str_escaped(s, key);
    if (err != BB_OK) return err;
    return obj_putc(s, ':');
}

// ============================================================================
// Public API
// ============================================================================

bb_err_t bb_http_resp_json_obj_begin(bb_http_request_t *req,
                                     bb_http_json_obj_stream_t *out)
{
    if (!req || !out) return BB_ERR_INVALID_ARG;

    bb_err_t err = bb_http_resp_set_type(req, "application/json");
    if (err != BB_OK) return err;

    memset(out, 0, sizeof(*out));
    out->_req  = req;
    out->_open = 1;

    err = bb_http_resp_send_chunk(req, "{", 1);
    if (err != BB_OK) {
        out->_err = err;
        return err;
    }
    return BB_OK;
}

bb_err_t bb_http_resp_json_obj_set_str(bb_http_json_obj_stream_t *stream,
                                       const char *key, const char *val)
{
    if (!stream) return BB_ERR_INVALID_ARG;
    if (!stream->_open) return BB_ERR_INVALID_STATE;
    if (stream->_err != BB_OK) return stream->_err;

    bb_err_t err = obj_emit_key(stream, key);
    if (err != BB_OK) return err;

    if (!val) {
        return obj_append(stream, "null", 4);
    }
    return obj_emit_str_escaped(stream, val);
}

bb_err_t bb_http_resp_json_obj_set_num(bb_http_json_obj_stream_t *stream,
                                       const char *key, double val)
{
    if (!stream) return BB_ERR_INVALID_ARG;
    if (!stream->_open) return BB_ERR_INVALID_STATE;
    if (stream->_err != BB_OK) return stream->_err;

    bb_err_t err = obj_emit_key(stream, key);
    if (err != BB_OK) return err;

    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%g", val);
    if (n < 0 || (size_t)n >= sizeof(buf)) n = (int)(sizeof(buf) - 1);
    return obj_append(stream, buf, (size_t)n);
}

bb_err_t bb_http_resp_json_obj_set_int(bb_http_json_obj_stream_t *stream,
                                       const char *key, int64_t val)
{
    if (!stream) return BB_ERR_INVALID_ARG;
    if (!stream->_open) return BB_ERR_INVALID_STATE;
    if (stream->_err != BB_OK) return stream->_err;

    bb_err_t err = obj_emit_key(stream, key);
    if (err != BB_OK) return err;

    char buf[24];
    int n = snprintf(buf, sizeof(buf), "%" PRId64, val);
    if (n < 0 || (size_t)n >= sizeof(buf)) n = (int)(sizeof(buf) - 1);
    return obj_append(stream, buf, (size_t)n);
}

bb_err_t bb_http_resp_json_obj_set_bool(bb_http_json_obj_stream_t *stream,
                                        const char *key, bool val)
{
    if (!stream) return BB_ERR_INVALID_ARG;
    if (!stream->_open) return BB_ERR_INVALID_STATE;
    if (stream->_err != BB_OK) return stream->_err;

    bb_err_t err = obj_emit_key(stream, key);
    if (err != BB_OK) return err;

    return obj_append(stream, val ? "true" : "false", val ? 4 : 5);
}

bb_err_t bb_http_resp_json_obj_set_null(bb_http_json_obj_stream_t *stream,
                                        const char *key)
{
    if (!stream) return BB_ERR_INVALID_ARG;
    if (!stream->_open) return BB_ERR_INVALID_STATE;
    if (stream->_err != BB_OK) return stream->_err;

    bb_err_t err = obj_emit_key(stream, key);
    if (err != BB_OK) return err;

    return obj_append(stream, "null", 4);
}

bb_err_t bb_http_resp_json_obj_set_obj_begin(bb_http_json_obj_stream_t *stream,
                                             const char *key)
{
    if (!stream) return BB_ERR_INVALID_ARG;
    if (!stream->_open) return BB_ERR_INVALID_STATE;
    if (stream->_err != BB_OK) return stream->_err;
    if (stream->_depth + 1 >= BB_JSON_OBJ_MAX_DEPTH) {
        stream->_err = BB_ERR_INVALID_ARG;
        return stream->_err;
    }

    bb_err_t err = obj_emit_key(stream, key);
    if (err != BB_OK) return err;

    err = obj_putc(stream, '{');
    if (err != BB_OK) return err;

    stream->_depth++;
    stream->_needs_comma[stream->_depth] = 0;
    return BB_OK;
}

bb_err_t bb_http_resp_json_obj_set_obj_end(bb_http_json_obj_stream_t *stream)
{
    if (!stream) return BB_ERR_INVALID_ARG;
    if (!stream->_open) return BB_ERR_INVALID_STATE;
    if (stream->_err != BB_OK) return stream->_err;
    if (stream->_depth == 0) {
        stream->_err = BB_ERR_INVALID_STATE;
        return stream->_err;
    }

    stream->_depth--;
    return obj_putc(stream, '}');
}

bb_err_t bb_http_resp_json_obj_set_arr_begin(bb_http_json_obj_stream_t *stream,
                                             const char *key)
{
    if (!stream) return BB_ERR_INVALID_ARG;
    if (!stream->_open) return BB_ERR_INVALID_STATE;
    if (stream->_err != BB_OK) return stream->_err;
    if (stream->_depth + 1 >= BB_JSON_OBJ_MAX_DEPTH) {
        stream->_err = BB_ERR_INVALID_ARG;
        return stream->_err;
    }

    bb_err_t err = obj_emit_key(stream, key);
    if (err != BB_OK) return err;

    err = obj_putc(stream, '[');
    if (err != BB_OK) return err;

    stream->_depth++;
    stream->_needs_comma[stream->_depth] = 0;
    return BB_OK;
}

bb_err_t bb_http_resp_json_obj_set_arr_end(bb_http_json_obj_stream_t *stream)
{
    if (!stream) return BB_ERR_INVALID_ARG;
    if (!stream->_open) return BB_ERR_INVALID_STATE;
    if (stream->_err != BB_OK) return stream->_err;
    if (stream->_depth == 0) {
        stream->_err = BB_ERR_INVALID_STATE;
        return stream->_err;
    }

    stream->_depth--;
    return obj_putc(stream, ']');
}

bb_err_t bb_http_resp_json_obj_end(bb_http_json_obj_stream_t *stream)
{
    if (!stream) return BB_ERR_INVALID_ARG;

    stream->_open = 0;

    // Append the closing brace to the buffer
    if (stream->_err == BB_OK) {
        obj_putc(stream, '}');
    }

    // Flush remaining buffer
    obj_flush(stream);

    // Terminate chunked response
    bb_http_request_t *req = (bb_http_request_t *)stream->_req;
    bb_err_t fin = bb_http_resp_send_chunk(req, NULL, 0);
    if (fin != BB_OK && stream->_err == BB_OK) {
        stream->_err = fin;
    }

    return stream->_err;
}
