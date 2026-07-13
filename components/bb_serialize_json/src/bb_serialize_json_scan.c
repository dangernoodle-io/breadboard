// Hand-rolled, no-heap, streaming JSON event scanner -- the read-side
// counterpart to bb_serialize_json.c's writer. One byte-driven
// grammar/depth state machine (`drive()`) backs both public entry points:
// bb_serialize_json_scan_bounded() is a thin wrapper that feeds the whole
// buffer to the streaming machinery in a single _feed() call, so by
// construction no token can ever straddle a chunk boundary in that mode --
// the "collapse" is a structural consequence of calling drive() exactly
// once, not a separate code path.
//
// Reassembly split (see bb_serialize_json.h for the full contract):
//   - keys, numbers, true/false/null -- ALWAYS fully reassembled into the
//     single shared `scratch` buffer (key bytes occupy the prefix, the
//     value that follows is appended directly after them; a value with no
//     key starts at scratch offset 0).
//   - string VALUES -- streamed as spans (never buffered); a run of plain
//     bytes is handed to the sink as a direct slice of the caller's input
//     chunk, and a decoded escape sequence is handed over as its own tiny
//     span from a small on-stack buffer. An escape that straddles a feed()
//     boundary is resumed via small fixed state fields (esc_phase/
//     esc_code_unit/esc_high_surrogate) rather than a byte-array scratch
//     copy -- functionally equivalent to buffering the raw escape bytes,
//     but avoids re-parsing hex digits out of a buffer on resume.

#include "bb_serialize_json.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Kconfig bridge -- CONFIG_BB_SERIALIZE_JSON_SCRATCH_MAX_BYTES -> a C
// default. Never shadow the generated symbol with a bare #ifndef.
// ---------------------------------------------------------------------------
#ifdef CONFIG_BB_SERIALIZE_JSON_SCRATCH_MAX_BYTES
#define BB_SERIALIZE_JSON_SCRATCH_MAX_BYTES CONFIG_BB_SERIALIZE_JSON_SCRATCH_MAX_BYTES
#else
#define BB_SERIALIZE_JSON_SCRATCH_MAX_BYTES 64
#endif

// ---------------------------------------------------------------------------
// Internal state machine -- cast the opaque bb_serialize_json_scan_ctx_t
// _state[] to this struct (same pattern as bb_release_manifest_stream_ctx_t
// / stream_state_t).
// ---------------------------------------------------------------------------

typedef enum {
    PH_ROOT,             // before the root value: skip ws, dispatch a value
    PH_AFTER_ROOT,        // root value complete: only trailing ws allowed
    PH_OBJ_KEY_OR_CLOSE,  // just after '{': a key string or '}' (empty obj)
    PH_OBJ_KEY_ONLY,      // just after ',' in an object: a key string only
    PH_KEY_STR,           // scanning an object member's key string
    PH_OBJ_COLON,         // key closed: expect ':'
    PH_OBJ_VALUE,         // colon consumed: dispatch the member's value
    PH_OBJ_AFTER_VALUE,   // member value complete: expect ',' or '}'
    PH_ARR_ELEM_OR_CLOSE, // just after '[': a value or ']' (empty array)
    PH_ARR_ELEM_ONLY,     // just after ',' in an array: a value only
    PH_ARR_AFTER_VALUE,   // element value complete: expect ',' or ']'
    PH_VAL_STR,           // scanning a string VALUE (streamed as spans)
    PH_VAL_NUM,           // scanning a number
    PH_VAL_LIT,           // scanning true/false/null
} phase_t;

typedef enum {
    NUM_START,       // just consumed a leading '-'; need >=1 digit
    NUM_INT_DIGITS,  // in the integer-part digit run (>=1 digit seen)
    NUM_FRAC_START,  // just consumed '.'; need >=1 digit
    NUM_FRAC_DIGITS,
    NUM_EXP_START,   // just consumed e/E; need an optional sign then a digit
    NUM_EXP_SIGN,    // just consumed a sign; need >=1 digit
    NUM_EXP_DIGITS,
} num_phase_t;

typedef enum { LIT_TRUE, LIT_FALSE, LIT_NULL } lit_kind_t;

// String-escape sub-state, shared by key and value string scanning (only
// one string can ever be mid-parse at a time -- see the file banner).
typedef enum {
    ESC_NONE,           // not mid-escape (normal run scanning)
    ESC_BACKSLASH,      // consumed '\', expect the escape-type byte
    ESC_U1, ESC_U2, ESC_U3, ESC_U4,             // \uXXXX hex digits (1st unit)
    ESC_LOW_BACKSLASH,  // high surrogate seen; expect '\' starting the pair
    ESC_LOW_U,          // expect 'u' of the low surrogate's \u escape
    ESC_LOW_U1, ESC_LOW_U2, ESC_LOW_U3, ESC_LOW_U4, // low surrogate hex digits
} esc_phase_t;

typedef struct {
    const bb_serialize_json_ingest_t *sink;

    phase_t  phase;
    bb_err_t err;  // sticky; BB_OK until the first error/sink-rejection

    // Entry-point mode this scan is running under -- drives which
    // provenance value a direct-span run flush emits (see
    // direct_span_provenance() below): true for bb_serialize_json_scan_bounded()
    // (the caller's buf outlives the call -> CALLER_STABLE), false for the
    // streaming bb_serialize_json_scan_begin()/_feed()/_end() entry points
    // (the span only outlives the current _feed() call -> CALLER_FEED_SCOPED).
    bool bounded_mode;

    bool    obj_stack[BB_SERIALIZE_MAX_DEPTH];  // true = array, false = object
    uint8_t depth;                              // count of open containers

    // Shared key/number/literal reassembly scratch: a completed key
    // occupies scratch[0..key_len), and the value that follows it (if a
    // number) is appended directly after, at scratch[key_len..scratch_len).
    char   scratch[BB_SERIALIZE_JSON_SCRATCH_MAX_BYTES];
    size_t scratch_len;
    size_t key_len;

    num_phase_t num_phase;

    lit_kind_t lit_kind;
    uint8_t    lit_matched;  // bytes of the target literal matched so far

    esc_phase_t esc_phase;
    uint16_t    esc_code_unit;      // \uXXXX code unit currently accumulating
    uint16_t    esc_high_surrogate; // stashed high surrogate awaiting its pair
} scan_state_t;

// Compile-time size check -- catches BB_SERIALIZE_JSON_SCAN_STATE_SIZE (in
// the public header) ever falling short of the real internal struct.
_Static_assert(sizeof(scan_state_t) <= BB_SERIALIZE_JSON_SCAN_STATE_SIZE,
                "BB_SERIALIZE_JSON_SCAN_STATE_SIZE too small for scan_state_t");

static inline scan_state_t *scan_state(bb_serialize_json_scan_ctx_t *ctx)
{
    return (scan_state_t *)(void *)ctx->_state;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void skip_ws(const char *chunk, size_t len, size_t *i)
{
    while (*i < len && is_ws(chunk[*i])) (*i)++;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Encodes one Unicode code point as UTF-8 into `out` (capacity >= 4);
// returns the byte count written.
static size_t utf8_encode(uint32_t cp, char *out)
{
    if (cp <= 0x7Fu) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FFu) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp <= 0xFFFFu) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

static const char *cur_key(scan_state_t *s)
{
    return s->key_len ? s->scratch : NULL;
}

// Returns the phase to resume in once a value (scalar or container close)
// at the current depth has just completed.
static phase_t after_value_phase(scan_state_t *s)
{
    if (s->depth == 0) return PH_AFTER_ROOT;
    return s->obj_stack[s->depth - 1] ? PH_ARR_AFTER_VALUE : PH_OBJ_AFTER_VALUE;
}

// Appends to the shared scratch buffer (key bytes, or number digits
// following an already-frozen key). BB_ERR_NO_SPACE on overflow -- never an
// OOB write.
static bb_err_t scratch_append(scan_state_t *s, const char *data, size_t n)
{
    if (s->scratch_len + n > sizeof(s->scratch)) return BB_ERR_NO_SPACE;
    memcpy(s->scratch + s->scratch_len, data, n);
    s->scratch_len += n;
    return BB_OK;
}

// The provenance value for a DIRECT-SPAN run flush (a slice of the caller's
// own input, never scanner scratch) -- depends on which entry point this
// scan is running under (see scan_state_t.bounded_mode): the caller's buf
// outlives the call in bounded mode (CALLER_STABLE), but a streaming
// _feed() chunk argument only outlives that one call (CALLER_FEED_SCOPED).
static bb_serialize_json_span_t direct_span_provenance(scan_state_t *s)
{
    return s->bounded_mode ? BB_SERIALIZE_JSON_SPAN_CALLER_STABLE
                            : BB_SERIALIZE_JSON_SPAN_CALLER_FEED_SCOPED;
}

// Delivers `n` decoded bytes for the string currently being scanned: for a
// key, appends into scratch (full reassembly); for a value, hands the span
// straight to the sink (streamed, never buffered). `provenance` is the
// span-provenance signal forwarded verbatim to the sink -- see
// bb_serialize_json_span_t's contract on
// bb_serialize_json_ingest_t.value_str_chunk (bb_serialize_json.h):
// CALLER_STABLE/CALLER_FEED_SCOPED (via direct_span_provenance()) for a
// direct slice of the caller's input, SCANNER_SCRATCH for decoded-escape
// scratch that dies when this call returns.
static bb_err_t emit_str_bytes(scan_state_t *s, const char *data, size_t n,
                                bool is_final, bool is_key,
                                bb_serialize_json_span_t provenance)
{
    if (is_key) return scratch_append(s, data, n);
    return s->sink->value_str_chunk(s->sink->ctx, cur_key(s), s->key_len, data, n, is_final,
                                     provenance);
}

// ---------------------------------------------------------------------------
// String scanning -- shared by object keys (PH_KEY_STR) and string values
// (PH_VAL_STR). Consumes bytes from `chunk` starting at `*i`, advancing
// `*i`, until either the closing quote is consumed (*closed=true), the
// chunk runs out mid-string (*closed=false, caller awaits more data via the
// next feed() call), or an error occurs.
// ---------------------------------------------------------------------------

// Processes exactly one byte of an in-progress escape sequence
// (s->esc_phase != ESC_NONE). Advances *i by one. Does NOT emit -- when the
// escape fully resolves, the decoded bytes are written to `out` (capacity
// >= 4) and `*out_n` is set > 0; otherwise `*out_n` is left at 0 (more
// bytes needed, possibly on a future feed() call). Emission is left to the
// caller (scan_string) so it can peek at the very next byte first and, if
// it is the closing quote, fold that into a single final call instead of a
// separate trailing empty one.
static bb_err_t scan_escape_byte(scan_state_t *s, const char *chunk, size_t *i,
                                  char *out, size_t *out_n)
{
    char c = chunk[*i];
    *out_n = 0;

    switch (s->esc_phase) {  // LCOV_EXCL_BR_LINE -- ESC_NONE arm is defensive (never dispatched here)
    case ESC_BACKSLASH: {
        char decoded;
        switch (c) {
        case '"':  decoded = '"';  break;
        case '\\': decoded = '\\'; break;
        case '/':  decoded = '/';  break;
        case 'b':  decoded = '\b'; break;
        case 'f':  decoded = '\f'; break;
        case 'n':  decoded = '\n'; break;
        case 'r':  decoded = '\r'; break;
        case 't':  decoded = '\t'; break;
        case 'u':
            (*i)++;
            s->esc_phase = ESC_U1;
            s->esc_code_unit = 0;
            return BB_OK;
        default:
            return BB_ERR_VALIDATION;
        }
        (*i)++;
        s->esc_phase = ESC_NONE;
        out[0] = decoded;
        *out_n = 1;
        return BB_OK;
    }

    case ESC_U1: case ESC_U2: case ESC_U3: case ESC_U4: {
        int hv = hex_val(c);
        if (hv < 0) return BB_ERR_VALIDATION;
        s->esc_code_unit = (uint16_t)((s->esc_code_unit << 4) | (unsigned)hv);
        (*i)++;
        if (s->esc_phase != ESC_U4) {
            s->esc_phase = (esc_phase_t)(s->esc_phase + 1);
            return BB_OK;
        }
        if (s->esc_code_unit >= 0xD800u && s->esc_code_unit <= 0xDBFFu) {
            // High surrogate: must be followed by a \uXXXX low surrogate.
            s->esc_high_surrogate = s->esc_code_unit;
            s->esc_phase = ESC_LOW_BACKSLASH;
            return BB_OK;
        }
        if (s->esc_code_unit >= 0xDC00u && s->esc_code_unit <= 0xDFFFu) {
            return BB_ERR_VALIDATION;  // unpaired low surrogate
        }
        s->esc_phase = ESC_NONE;
        *out_n = utf8_encode(s->esc_code_unit, out);
        return BB_OK;
    }

    case ESC_LOW_BACKSLASH:
        if (c != '\\') return BB_ERR_VALIDATION;  // unpaired high surrogate
        (*i)++;
        s->esc_phase = ESC_LOW_U;
        return BB_OK;

    case ESC_LOW_U:
        if (c != 'u') return BB_ERR_VALIDATION;  // unpaired high surrogate
        (*i)++;
        s->esc_phase = ESC_LOW_U1;
        s->esc_code_unit = 0;
        return BB_OK;

    case ESC_LOW_U1: case ESC_LOW_U2: case ESC_LOW_U3: case ESC_LOW_U4: {
        int hv = hex_val(c);
        if (hv < 0) return BB_ERR_VALIDATION;
        s->esc_code_unit = (uint16_t)((s->esc_code_unit << 4) | (unsigned)hv);
        (*i)++;
        if (s->esc_phase != ESC_LOW_U4) {
            s->esc_phase = (esc_phase_t)(s->esc_phase + 1);
            return BB_OK;
        }
        if (s->esc_code_unit < 0xDC00u || s->esc_code_unit > 0xDFFFu) {
            return BB_ERR_VALIDATION;  // invalid low surrogate
        }
        {
            uint32_t cp = 0x10000u
                + (((uint32_t)s->esc_high_surrogate - 0xD800u) << 10)
                + ((uint32_t)s->esc_code_unit - 0xDC00u);
            s->esc_phase = ESC_NONE;
            *out_n = utf8_encode(cp, out);
            return BB_OK;
        }
    }

    default:  // LCOV_EXCL_LINE -- ESC_NONE never dispatched here (see caller guard above)
        return BB_ERR_VALIDATION;  // LCOV_EXCL_LINE -- ESC_NONE never dispatched here
    }
}

static bb_err_t scan_string(scan_state_t *s, const char *chunk, size_t len, size_t *i,
                             bool is_key, bool *closed)
{
    *closed = false;

    while (*i < len) {
        if (s->esc_phase != ESC_NONE) {
            char   decoded[4];
            size_t decoded_n = 0;
            bb_err_t rc = scan_escape_byte(s, chunk, i, decoded, &decoded_n);
            if (rc != BB_OK) return rc;
            if (decoded_n == 0) continue;  // still mid-escape, need more bytes

            // Peek: if the very next byte (already in this chunk) is the
            // closing quote, fold it into a single final call instead of a
            // separate trailing empty one. Either way `decoded` is on-stack
            // scratch -- SCANNER_SCRATCH, regardless of is_final (see the
            // bb_serialize_json_span_t contract: call count/is_final is not
            // a proxy for span safety).
            if (*i < len && chunk[*i] == '"') {
                (*i)++;
                rc = emit_str_bytes(s, decoded, decoded_n, true, is_key,
                                     BB_SERIALIZE_JSON_SPAN_SCANNER_SCRATCH);
                if (rc != BB_OK) return rc;
                *closed = true;
                return BB_OK;
            }
            rc = emit_str_bytes(s, decoded, decoded_n, false, is_key,
                                 BB_SERIALIZE_JSON_SPAN_SCANNER_SCRATCH);
            if (rc != BB_OK) return rc;
            continue;
        }

        size_t start = *i;
        while (*i < len) {
            unsigned char c = (unsigned char)chunk[*i];
            if (c == '"' || c == '\\') break;
            if (c < 0x20u) return BB_ERR_VALIDATION;  // unescaped control char
            (*i)++;
        }
        size_t run_len = *i - start;

        if (*i >= len) {
            // Chunk exhausted mid-run -- flush what we have and await more.
            // run_len is always > 0 here: the caller (drive()) only ever
            // enters this function with *i < len, so reaching this branch
            // with run_len == 0 would require *i == len already at the top
            // of this loop iteration, which the loop guard above precludes.
            if (run_len > 0) {  // LCOV_EXCL_BR_LINE -- false arm is unreachable, see above
                bb_err_t rc = emit_str_bytes(s, chunk + start, run_len, false, is_key,
                                              direct_span_provenance(s));
                if (rc != BB_OK) return rc;
            }
            return BB_OK;
        }

        if (chunk[*i] == '"') {
            (*i)++;
            bb_err_t rc = emit_str_bytes(s, chunk + start, run_len, true, is_key,
                                          direct_span_provenance(s));
            if (rc != BB_OK) return rc;
            *closed = true;
            return BB_OK;
        }

        // chunk[*i] == '\\'
        if (run_len > 0) {
            bb_err_t rc = emit_str_bytes(s, chunk + start, run_len, false, is_key,
                                          direct_span_provenance(s));
            if (rc != BB_OK) return rc;
        }
        (*i)++;
        s->esc_phase = ESC_BACKSLASH;
    }

    return BB_OK;
}

// ---------------------------------------------------------------------------
// Number scanning
// ---------------------------------------------------------------------------

static bb_err_t scan_number(scan_state_t *s, const char *chunk, size_t len, size_t *i,
                             bool *finished)
{
    *finished = false;

    while (*i < len) {
        char        c = chunk[*i];
        bool        digit = (c >= '0' && c <= '9');
        num_phase_t next_phase = s->num_phase;

        // Pure phase-transition table: every arm below either returns
        // (malformed, or a terminal byte that ends the number without
        // consuming it) or falls out having set `next_phase` -- consuming
        // `c` and advancing is handled once, below, at a single shared
        // choke point rather than repeated per-transition.
        switch (s->num_phase) {  // LCOV_EXCL_BR_LINE -- default arm is defensive (exhaustive enum)
        case NUM_START:
            if (!digit) return BB_ERR_VALIDATION;
            next_phase = NUM_INT_DIGITS;
            break;

        case NUM_INT_DIGITS:
            if (digit) break;
            if (c == '.') { next_phase = NUM_FRAC_START; break; }
            if (c == 'e' || c == 'E') { next_phase = NUM_EXP_START; break; }
            *finished = true;
            return BB_OK;

        case NUM_FRAC_START:
            if (!digit) return BB_ERR_VALIDATION;
            next_phase = NUM_FRAC_DIGITS;
            break;

        case NUM_FRAC_DIGITS:
            if (digit) break;
            if (c == 'e' || c == 'E') { next_phase = NUM_EXP_START; break; }
            *finished = true;
            return BB_OK;

        case NUM_EXP_START:
            if (c == '+' || c == '-') { next_phase = NUM_EXP_SIGN; break; }
            if (digit) { next_phase = NUM_EXP_DIGITS; break; }
            return BB_ERR_VALIDATION;

        case NUM_EXP_SIGN:
            if (!digit) return BB_ERR_VALIDATION;
            next_phase = NUM_EXP_DIGITS;
            break;

        case NUM_EXP_DIGITS:
            if (digit) break;
            *finished = true;
            return BB_OK;

        default:  // LCOV_EXCL_LINE -- exhaustive enum, defensive
            return BB_ERR_VALIDATION;  // LCOV_EXCL_LINE -- exhaustive enum, defensive
        }

        bb_err_t rc = scratch_append(s, &c, 1);
        if (rc != BB_OK) return rc;
        s->num_phase = next_phase;
        (*i)++;
    }

    return BB_OK;  // chunk exhausted, not finished
}

static bool num_phase_is_terminal(num_phase_t p)
{
    return p == NUM_INT_DIGITS || p == NUM_FRAC_DIGITS || p == NUM_EXP_DIGITS;
}

static bb_err_t finalize_number(scan_state_t *s)
{
    const char *key = cur_key(s);
    const char *num = s->scratch + s->key_len;
    size_t      num_len = s->scratch_len - s->key_len;

    bb_err_t rc = s->sink->value_num(s->sink->ctx, key, s->key_len, num, num_len);
    if (rc != BB_OK) return rc;
    s->phase = after_value_phase(s);
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Literal (true/false/null) scanning -- no scratch needed: the sink only
// ever needs the decoded bool/null, never the raw text, so a matched-count
// index against the target literal is sufficient (and inherently resumable
// across a feed() boundary, since it lives in scan_state_t).
// ---------------------------------------------------------------------------

static bb_err_t scan_literal(scan_state_t *s, const char *chunk, size_t len, size_t *i,
                              bool *finished)
{
    static const char *const lit_text[3] = { "true", "false", "null" };
    const char *target = lit_text[s->lit_kind];
    size_t      target_len = strlen(target);

    *finished = false;
    while (*i < len) {
        if (chunk[*i] != target[s->lit_matched]) return BB_ERR_VALIDATION;
        s->lit_matched++;
        (*i)++;
        if (s->lit_matched == target_len) {
            *finished = true;
            return BB_OK;
        }
    }
    return BB_OK;
}

static bb_err_t emit_literal(scan_state_t *s)
{
    const char *key = cur_key(s);
    if (s->lit_kind == LIT_NULL) return s->sink->value_null(s->sink->ctx, key, s->key_len);
    return s->sink->value_bool(s->sink->ctx, key, s->key_len, s->lit_kind == LIT_TRUE);
}

// ---------------------------------------------------------------------------
// Container open/close
// ---------------------------------------------------------------------------

static bb_err_t close_container(scan_state_t *s)
{
    bool is_arr = s->obj_stack[s->depth - 1];
    s->depth--;
    bb_err_t rc = is_arr ? s->sink->end_arr(s->sink->ctx) : s->sink->end_obj(s->sink->ctx);
    if (rc != BB_OK) return rc;
    s->phase = after_value_phase(s);
    return BB_OK;
}

// Dispatches on the first byte of a value ('"', '{', '[', a number start,
// or a literal start). `keyed` is false for a root/array-element value
// (scratch is reset for a fresh reassembly), true for an object member's
// value (the key already occupies scratch[0..key_len)).
static bb_err_t dispatch_value_start(scan_state_t *s, const char *chunk, size_t len,
                                      size_t *i, bool keyed)
{
    if (!keyed) {
        s->key_len = 0;
        s->scratch_len = 0;
    }

    char        c = chunk[*i];
    const char *key = cur_key(s);

    switch (c) {
    case '"':
        (*i)++;
        s->esc_phase = ESC_NONE;
        s->phase = PH_VAL_STR;
        return BB_OK;

    case '{':
        if (s->depth >= BB_SERIALIZE_MAX_DEPTH) return BB_ERR_NO_SPACE;
        {
            bb_err_t rc = s->sink->begin_obj(s->sink->ctx, key, s->key_len);
            if (rc != BB_OK) return rc;
        }
        s->obj_stack[s->depth] = false;
        s->depth++;
        (*i)++;
        s->phase = PH_OBJ_KEY_OR_CLOSE;
        return BB_OK;

    case '[':
        if (s->depth >= BB_SERIALIZE_MAX_DEPTH) return BB_ERR_NO_SPACE;
        {
            bb_err_t rc = s->sink->begin_arr(s->sink->ctx, key, s->key_len);
            if (rc != BB_OK) return rc;
        }
        s->obj_stack[s->depth] = true;
        s->depth++;
        (*i)++;
        s->phase = PH_ARR_ELEM_OR_CLOSE;
        return BB_OK;

    case '-':
        {
            bb_err_t rc = scratch_append(s, &c, 1);
            if (rc != BB_OK) return rc;
        }
        s->num_phase = NUM_START;
        s->phase = PH_VAL_NUM;
        (*i)++;
        return BB_OK;

    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        {
            bb_err_t rc = scratch_append(s, &c, 1);
            if (rc != BB_OK) return rc;
        }
        s->num_phase = NUM_INT_DIGITS;
        s->phase = PH_VAL_NUM;
        (*i)++;
        return BB_OK;

    case 't':
        s->lit_kind = LIT_TRUE;
        s->lit_matched = 1;
        s->phase = PH_VAL_LIT;
        (*i)++;
        return BB_OK;

    case 'f':
        s->lit_kind = LIT_FALSE;
        s->lit_matched = 1;
        s->phase = PH_VAL_LIT;
        (*i)++;
        return BB_OK;

    case 'n':
        s->lit_kind = LIT_NULL;
        s->lit_matched = 1;
        s->phase = PH_VAL_LIT;
        (*i)++;
        return BB_OK;

    default:
        return BB_ERR_VALIDATION;
    }
}

// ---------------------------------------------------------------------------
// Main drive loop -- processes as much of `chunk` as forms complete tokens,
// returning BB_OK when it needs more data (or the whole chunk is
// consumed), or the first error/sink-rejection encountered.
// ---------------------------------------------------------------------------

static bb_err_t drive(scan_state_t *s, const char *chunk, size_t len, size_t *i)
{
    while (*i < len) {
        bb_err_t rc;
        char     c;

        switch (s->phase) {  // LCOV_EXCL_BR_LINE -- default arm is defensive (exhaustive enum)
        case PH_ROOT:
            skip_ws(chunk, len, i);
            if (*i >= len) return BB_OK;
            rc = dispatch_value_start(s, chunk, len, i, false);
            if (rc != BB_OK) return rc;
            break;

        case PH_AFTER_ROOT:
            skip_ws(chunk, len, i);
            if (*i >= len) return BB_OK;
            return BB_ERR_VALIDATION;  // trailing garbage after the root value

        case PH_OBJ_KEY_OR_CLOSE:
        case PH_OBJ_KEY_ONLY:
            skip_ws(chunk, len, i);
            if (*i >= len) return BB_OK;
            c = chunk[*i];
            if (c == '}' && s->phase == PH_OBJ_KEY_OR_CLOSE) {
                rc = close_container(s);
                if (rc != BB_OK) return rc;
                (*i)++;
                break;
            }
            if (c != '"') return BB_ERR_VALIDATION;
            (*i)++;
            s->scratch_len = 0;
            s->key_len = 0;
            s->esc_phase = ESC_NONE;
            s->phase = PH_KEY_STR;
            break;

        case PH_KEY_STR: {
            bool closed = false;
            rc = scan_string(s, chunk, len, i, true, &closed);
            if (rc != BB_OK) return rc;
            if (!closed) return BB_OK;
            s->key_len = s->scratch_len;
            s->phase = PH_OBJ_COLON;
            break;
        }

        case PH_OBJ_COLON:
            skip_ws(chunk, len, i);
            if (*i >= len) return BB_OK;
            if (chunk[*i] != ':') return BB_ERR_VALIDATION;
            (*i)++;
            s->phase = PH_OBJ_VALUE;
            break;

        case PH_OBJ_VALUE:
            skip_ws(chunk, len, i);
            if (*i >= len) return BB_OK;
            rc = dispatch_value_start(s, chunk, len, i, true);
            if (rc != BB_OK) return rc;
            break;

        case PH_OBJ_AFTER_VALUE:
            skip_ws(chunk, len, i);
            if (*i >= len) return BB_OK;
            c = chunk[*i];
            if (c == ',') { (*i)++; s->phase = PH_OBJ_KEY_ONLY; break; }
            if (c == '}') {
                rc = close_container(s);
                if (rc != BB_OK) return rc;
                (*i)++;
                break;
            }
            return BB_ERR_VALIDATION;

        case PH_ARR_ELEM_OR_CLOSE:
        case PH_ARR_ELEM_ONLY:
            skip_ws(chunk, len, i);
            if (*i >= len) return BB_OK;
            c = chunk[*i];
            if (c == ']' && s->phase == PH_ARR_ELEM_OR_CLOSE) {
                rc = close_container(s);
                if (rc != BB_OK) return rc;
                (*i)++;
                break;
            }
            rc = dispatch_value_start(s, chunk, len, i, false);
            if (rc != BB_OK) return rc;
            break;

        case PH_ARR_AFTER_VALUE:
            skip_ws(chunk, len, i);
            if (*i >= len) return BB_OK;
            c = chunk[*i];
            if (c == ',') { (*i)++; s->phase = PH_ARR_ELEM_ONLY; break; }
            if (c == ']') {
                rc = close_container(s);
                if (rc != BB_OK) return rc;
                (*i)++;
                break;
            }
            return BB_ERR_VALIDATION;

        case PH_VAL_STR: {
            bool closed = false;
            rc = scan_string(s, chunk, len, i, false, &closed);
            if (rc != BB_OK) return rc;
            if (!closed) return BB_OK;
            s->phase = after_value_phase(s);
            break;
        }

        case PH_VAL_NUM: {
            bool finished = false;
            rc = scan_number(s, chunk, len, i, &finished);
            if (rc != BB_OK) return rc;
            if (!finished) return BB_OK;
            rc = finalize_number(s);
            if (rc != BB_OK) return rc;
            break;
        }

        case PH_VAL_LIT: {
            bool finished = false;
            rc = scan_literal(s, chunk, len, i, &finished);
            if (rc != BB_OK) return rc;
            if (!finished) return BB_OK;
            rc = emit_literal(s);
            if (rc != BB_OK) return rc;
            s->phase = after_value_phase(s);
            break;
        }

        default:  // LCOV_EXCL_LINE -- exhaustive enum, defensive
            return BB_ERR_INVALID_STATE;  // LCOV_EXCL_LINE -- exhaustive enum, defensive
        }
    }

    return BB_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Shared by the public bb_serialize_json_scan_begin() (always streaming --
// bounded_mode=false) and bb_serialize_json_scan_bounded() (bounded_mode=true,
// set directly rather than via the public entry point, since the mode is
// fixed for the lifetime of the scan and must be known before the first
// direct-span run flush -- see scan_state_t.bounded_mode).
static bb_err_t scan_begin_internal(bb_serialize_json_scan_ctx_t *ctx,
                                     const bb_serialize_json_ingest_t *sink, bool bounded_mode)
{
    scan_state_t *s = scan_state(ctx);
    memset(s, 0, sizeof(*s));
    s->sink = sink;
    s->bounded_mode = bounded_mode;
    return BB_OK;
}

bb_err_t bb_serialize_json_scan_begin(bb_serialize_json_scan_ctx_t *ctx,
                                       const bb_serialize_json_ingest_t *sink)
{
    return scan_begin_internal(ctx, sink, false);
}

bb_err_t bb_serialize_json_scan_feed(bb_serialize_json_scan_ctx_t *ctx,
                                      const char *chunk, size_t chunk_len)
{
    scan_state_t *s = scan_state(ctx);
    if (s->err != BB_OK) return s->err;

    size_t   i = 0;
    bb_err_t rc = drive(s, chunk, chunk_len, &i);
    if (rc != BB_OK) s->err = rc;
    return rc;
}

bb_err_t bb_serialize_json_scan_end(bb_serialize_json_scan_ctx_t *ctx)
{
    scan_state_t *s = scan_state(ctx);
    if (s->err != BB_OK) return s->err;

    if (s->phase == PH_VAL_NUM) {
        if (!num_phase_is_terminal(s->num_phase)) {
            s->err = BB_ERR_INVALID_STATE;
            return s->err;
        }
        bb_err_t rc = finalize_number(s);
        if (rc != BB_OK) {
            s->err = rc;
            return rc;
        }
    }

    if (s->phase != PH_AFTER_ROOT) {
        s->err = BB_ERR_INVALID_STATE;
        return s->err;
    }

    return BB_OK;
}

bb_err_t bb_serialize_json_scan_bounded(const char *buf, size_t len,
                                         const bb_serialize_json_ingest_t *sink)
{
    bb_serialize_json_scan_ctx_t ctx;

    bb_err_t rc = scan_begin_internal(&ctx, sink, true);
    if (rc != BB_OK) return rc;  // LCOV_EXCL_LINE -- scan_begin_internal never fails today

    rc = bb_serialize_json_scan_feed(&ctx, buf, len);
    if (rc != BB_OK) return rc;

    return bb_serialize_json_scan_end(&ctx);
}
