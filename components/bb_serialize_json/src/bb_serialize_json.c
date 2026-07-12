// Hand-rolled, no-heap, bounded-buffer JSON bb_serialize_emit_t backend.
// All writes funnel through bb_json_put()/bb_json_putc() so a single
// capacity check (in bb_json_put) governs every byte written -- overflow
// sets a sticky error and stops copying immediately, never a partial write.

#include "bb_serialize_json.h"

#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Kconfig bridge -- CONFIG_BB_SERIALIZE_JSON_F64_DECIMALS -> a C default.
// Never shadow the generated symbol with a bare #ifndef.
// ---------------------------------------------------------------------------
#ifdef CONFIG_BB_SERIALIZE_JSON_F64_DECIMALS
#define BB_SERIALIZE_JSON_F64_DECIMALS CONFIG_BB_SERIALIZE_JSON_F64_DECIMALS
#else
#define BB_SERIALIZE_JSON_F64_DECIMALS 6
#endif

// 10^N table, N in [0, 15] -- covers the Kconfig knob's full range.
static const double s_pow10[16] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7,
    1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
};

// 2^64 -- the boundary above which a double's integer part no longer fits
// a uint64_t (used as the f64 documented-range-limit check).
#define BB_JSON_TWO_POW_64 18446744073709551616.0

// ---------------------------------------------------------------------------
// Low-level bounded writers -- every byte written funnels through these.
// ---------------------------------------------------------------------------

static void bb_json_put(bb_serialize_json_ctx_t *ctx, const char *s, size_t n)
{
    if (ctx->err != BB_OK) return;
    if (ctx->len + n > ctx->cap) {
        ctx->err = BB_ERR_NO_SPACE;
        return;
    }
    memcpy(ctx->buf + ctx->len, s, n);
    ctx->len += n;
}

static void bb_json_putc(bb_serialize_json_ctx_t *ctx, char ch)
{
    bb_json_put(ctx, &ch, 1);
}

static char bb_json_hex_nibble(unsigned v)
{
    return (v < 10) ? (char)('0' + v) : (char)('a' + (v - 10));
}

// Escapes (s, len) per JSON string-content rules. `/` is left raw; bytes
// >= 0x20 (other than '"'/'\\') pass through verbatim (no UTF-8
// validation). Each escape sequence is written by a single put() call so
// overflow can never leave a half-written escape.
static void bb_json_escape_write(bb_serialize_json_ctx_t *ctx, const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  bb_json_put(ctx, "\\\"", 2); break;
        case '\\': bb_json_put(ctx, "\\\\", 2); break;
        case '\b': bb_json_put(ctx, "\\b", 2); break;
        case '\f': bb_json_put(ctx, "\\f", 2); break;
        case '\n': bb_json_put(ctx, "\\n", 2); break;
        case '\r': bb_json_put(ctx, "\\r", 2); break;
        case '\t': bb_json_put(ctx, "\\t", 2); break;
        default:
            if (c < 0x20) {
                char esc[6] = { '\\', 'u', '0', '0',
                                bb_json_hex_nibble((unsigned)(c >> 4) & 0xFU),
                                bb_json_hex_nibble((unsigned)c & 0xFU) };
                bb_json_put(ctx, esc, sizeof(esc));
            } else {
                bb_json_putc(ctx, (char)c);
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Number formatting -- hand-rolled, no snprintf, no locale.
// ---------------------------------------------------------------------------

static void bb_json_write_u64(bb_serialize_json_ctx_t *ctx, uint64_t v)
{
    char digits[20];  // UINT64_MAX has 20 decimal digits
    int  n = 0;
    do {
        digits[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v > 0);

    char out[20];
    for (int i = 0; i < n; i++) out[i] = digits[n - 1 - i];
    bb_json_put(ctx, out, (size_t)n);
}

static void bb_json_write_i64(bb_serialize_json_ctx_t *ctx, int64_t v)
{
    if (v < 0) {
        bb_json_putc(ctx, '-');
        // Negate as unsigned so INT64_MIN (whose magnitude overflows
        // int64_t) is handled correctly.
        uint64_t mag = (uint64_t)(-(v + 1)) + 1;
        bb_json_write_u64(ctx, mag);
    } else {
        bb_json_write_u64(ctx, (uint64_t)v);
    }
}

// Fixed-width zero-padded fractional digit write (single put() call).
static void bb_json_write_frac_digits(bb_serialize_json_ctx_t *ctx, uint64_t frac, int n_digits)
{
    char out[16];
    for (int i = n_digits - 1; i >= 0; i--) {
        out[i] = (char)('0' + (frac % 10));
        frac /= 10;
    }
    bb_json_put(ctx, out, (size_t)n_digits);
}

// Deterministic fixed-decimal double formatting: N fractional digits
// (BB_SERIALIZE_JSON_F64_DECIMALS), round-to-nearest via scale-and-round,
// with the rounding carry propagated into the integer part when it
// overflows the fractional scale. NaN/Inf and magnitudes whose integer
// part exceeds uint64_t range emit `null` -- a documented limit, not a
// sticky error (one bad sample must not fail the whole document).
static void bb_json_write_f64(bb_serialize_json_ctx_t *ctx, double v)
{
    if (isnan(v) || isinf(v)) {
        bb_json_put(ctx, "null", 4);
        return;
    }

    bool   neg = v < 0.0;
    double mag = neg ? -v : v;

    if (mag >= BB_JSON_TWO_POW_64) {
        bb_json_put(ctx, "null", 4);
        return;
    }

    uint64_t int_part = (uint64_t)mag;
    double   frac = mag - (double)int_part;

    double   scale = s_pow10[BB_SERIALIZE_JSON_F64_DECIMALS];
    uint64_t scale_i = (uint64_t)scale;
    uint64_t frac_i = (uint64_t)(frac * scale + 0.5);
    if (frac_i >= scale_i) {
        frac_i -= scale_i;
        int_part += 1;
    }

    if (neg) bb_json_putc(ctx, '-');
    bb_json_write_u64(ctx, int_part);
    bb_json_putc(ctx, '.');
    bb_json_write_frac_digits(ctx, frac_i, BB_SERIALIZE_JSON_F64_DECIMALS);
}

// ---------------------------------------------------------------------------
// Level stack (comma / key-vs-unkeyed bookkeeping)
// ---------------------------------------------------------------------------

// `stack` is sized 2*BB_SERIALIZE_MAX_DEPTH+2 -- a walker recursion level
// can cost TWO JSON containers (an arr-of-obj field's array frame plus,
// per element, an object frame), not one. This runtime check is
// belt-and-suspenders against that sizing ever being wrong (or the walker's
// depth cap changing): if the push would exceed capacity, set the sticky
// error and return WITHOUT writing -- never an OOB write into `stack`.
static void bb_json_push_level(bb_serialize_json_ctx_t *ctx, bool is_array)
{
    if (ctx->err != BB_OK) return;

    size_t next = (size_t)ctx->depth + 1;
    if (next >= (sizeof(ctx->stack) / sizeof(ctx->stack[0]))) {
        ctx->err = BB_ERR_NO_SPACE;
        return;
    }

    ctx->depth = (uint8_t)next;
    ctx->stack[ctx->depth].is_array = is_array;
    ctx->stack[ctx->depth].have_child = false;
}

static void bb_json_pop_level(bb_serialize_json_ctx_t *ctx)
{
    // Defensive against an unbalanced end_obj/end_arr driven manually via
    // the raw bb_serialize_json_emit() vtable (exported for manual drive) --
    // a bare `ctx->depth--` on a uint8_t would wrap 0 -> 255, an OOB index
    // on the very next push/pre-value.
    if (ctx->depth) ctx->depth--;
}

// Called before every scalar value and every begin_obj/begin_arr. Writes
// the leading comma (if a prior sibling exists) and, in an object context,
// the `"key":` prefix. `key` is NULL for array elements (nothing extra
// written) and -- defensively -- for a malformed object member (writes an
// empty key rather than dereferencing NULL).
static void bb_json_pre_value(bb_serialize_json_ctx_t *ctx, const char *key)
{
    bb_serialize_json_level_t *lvl = &ctx->stack[ctx->depth];
    if (lvl->have_child) {
        bb_json_putc(ctx, ',');
    } else {
        lvl->have_child = true;
    }

    if (!lvl->is_array) {
        bb_json_putc(ctx, '"');
        if (key) bb_json_escape_write(ctx, key, strlen(key));
        bb_json_put(ctx, "\":", 2);
    }
}

// ---------------------------------------------------------------------------
// bb_serialize_emit_t callbacks
// ---------------------------------------------------------------------------

static void bb_json_cb_begin_obj(void *vctx, const char *key)
{
    bb_serialize_json_ctx_t *ctx = (bb_serialize_json_ctx_t *)vctx;
    bb_json_pre_value(ctx, key);
    bb_json_putc(ctx, '{');
    bb_json_push_level(ctx, false);
}

static void bb_json_cb_end_obj(void *vctx)
{
    bb_serialize_json_ctx_t *ctx = (bb_serialize_json_ctx_t *)vctx;
    bb_json_putc(ctx, '}');
    bb_json_pop_level(ctx);
}

static void bb_json_cb_begin_arr(void *vctx, const char *key)
{
    bb_serialize_json_ctx_t *ctx = (bb_serialize_json_ctx_t *)vctx;
    bb_json_pre_value(ctx, key);
    bb_json_putc(ctx, '[');
    bb_json_push_level(ctx, true);
}

static void bb_json_cb_end_arr(void *vctx)
{
    bb_serialize_json_ctx_t *ctx = (bb_serialize_json_ctx_t *)vctx;
    bb_json_putc(ctx, ']');
    bb_json_pop_level(ctx);
}

static void bb_json_cb_emit_i64(void *vctx, const char *key, int64_t v)
{
    bb_serialize_json_ctx_t *ctx = (bb_serialize_json_ctx_t *)vctx;
    bb_json_pre_value(ctx, key);
    bb_json_write_i64(ctx, v);
}

static void bb_json_cb_emit_u64(void *vctx, const char *key, uint64_t v)
{
    bb_serialize_json_ctx_t *ctx = (bb_serialize_json_ctx_t *)vctx;
    bb_json_pre_value(ctx, key);
    bb_json_write_u64(ctx, v);
}

static void bb_json_cb_emit_f64(void *vctx, const char *key, double v)
{
    bb_serialize_json_ctx_t *ctx = (bb_serialize_json_ctx_t *)vctx;
    bb_json_pre_value(ctx, key);
    bb_json_write_f64(ctx, v);
}

static void bb_json_cb_emit_bool(void *vctx, const char *key, bool v)
{
    bb_serialize_json_ctx_t *ctx = (bb_serialize_json_ctx_t *)vctx;
    bb_json_pre_value(ctx, key);
    if (v) {
        bb_json_put(ctx, "true", 4);
    } else {
        bb_json_put(ctx, "false", 5);
    }
}

static void bb_json_cb_emit_str(void *vctx, const char *key, const char *s, size_t len)
{
    bb_serialize_json_ctx_t *ctx = (bb_serialize_json_ctx_t *)vctx;
    bb_json_pre_value(ctx, key);
    bb_json_putc(ctx, '"');
    bb_json_escape_write(ctx, s, len);
    bb_json_putc(ctx, '"');
}

static void bb_json_cb_emit_null(void *vctx, const char *key)
{
    bb_serialize_json_ctx_t *ctx = (bb_serialize_json_ctx_t *)vctx;
    bb_json_pre_value(ctx, key);
    bb_json_put(ctx, "null", 4);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void bb_serialize_json_ctx_init(bb_serialize_json_ctx_t *ctx, char *buf, size_t cap)
{
    ctx->buf = buf;
    ctx->cap = cap;
    ctx->len = 0;
    ctx->err = BB_OK;
    ctx->depth = 0;
    ctx->stack[0].is_array = false;
    ctx->stack[0].have_child = false;
}

bb_serialize_emit_t bb_serialize_json_emit(bb_serialize_json_ctx_t *ctx)
{
    bb_serialize_emit_t emit = {
        .format_id = BB_FORMAT_JSON,
        .ctx = ctx,
        .begin_obj = bb_json_cb_begin_obj,
        .end_obj = bb_json_cb_end_obj,
        .begin_arr = bb_json_cb_begin_arr,
        .end_arr = bb_json_cb_end_arr,
        .emit_i64 = bb_json_cb_emit_i64,
        .emit_u64 = bb_json_cb_emit_u64,
        .emit_f64 = bb_json_cb_emit_f64,
        .emit_bool = bb_json_cb_emit_bool,
        .emit_str = bb_json_cb_emit_str,
        .emit_null = bb_json_cb_emit_null,
    };
    return emit;
}

bb_err_t bb_serialize_json_render(const bb_serialize_desc_t *desc, const void *snap,
                                   char *buf, size_t cap, size_t *out_len)
{
    if (cap == 0) return BB_ERR_NO_SPACE;

    // Reserve the final byte for the NUL terminator -- the writer itself
    // never accounts for it.
    bb_serialize_json_ctx_t ctx;
    bb_serialize_json_ctx_init(&ctx, buf, cap - 1);

    bb_serialize_emit_t emit = bb_serialize_json_emit(&ctx);

    bb_json_putc(&ctx, '{');
    bb_serialize_walk(desc, snap, &emit);
    bb_json_putc(&ctx, '}');

    if (ctx.err != BB_OK) {
        buf[0] = '\0';
        if (out_len) *out_len = 0;
        return ctx.err;
    }

    buf[ctx.len] = '\0';
    if (out_len) *out_len = ctx.len;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Worst-case sizing bound -- a pure function of the descriptor only (no
// snapshot instance). Mirrors bb_serialize_walk()'s BB_SERIALIZE_MAX_DEPTH
// recursion guard exactly so the bound never diverges from actual walk
// behavior at the depth cap.
// ---------------------------------------------------------------------------

static size_t bb_json_bound_fields(const bb_serialize_field_t *fields, uint16_t n_fields,
                                    unsigned depth);

// Worst-case bytes for one array element (STR or OBJ shape).
static size_t bb_json_bound_elem(const bb_serialize_field_t *f, unsigned depth)
{
    if (f->elem_type == BB_TYPE_STR) {
        if (f->max_len == 0) return SIZE_MAX;
        return (size_t)6 * f->max_len + 2;
    }
    // BB_TYPE_OBJ
    if (depth >= BB_SERIALIZE_MAX_DEPTH) return 2;  // depth-capped: "{}"
    size_t c = bb_json_bound_fields(f->children, f->n_children, depth + 1);
    if (c == SIZE_MAX) return SIZE_MAX;
    return 2 + c;
}

// Worst-case bytes for one field's *value* (no key/comma).
static size_t bb_json_bound_value(const bb_serialize_field_t *f, unsigned depth)
{
    switch (f->type) {
    case BB_TYPE_I64:
    case BB_TYPE_U64:
        return 20;  // UINT64_MAX / INT64_MIN worst case
    case BB_TYPE_F64:
        return 24 + BB_SERIALIZE_JSON_F64_DECIMALS;
    case BB_TYPE_BOOL:
        return 5;  // "false"
    case BB_TYPE_STR:
    case BB_TYPE_STR_N:
        if (f->max_len == 0) return SIZE_MAX;
        return (size_t)6 * f->max_len + 2;
    case BB_TYPE_OBJ: {
        if (depth >= BB_SERIALIZE_MAX_DEPTH) return 2;  // "{}"
        size_t c = bb_json_bound_fields(f->children, f->n_children, depth + 1);
        if (c == SIZE_MAX) return SIZE_MAX;
        return 2 + c;
    }
    case BB_TYPE_ARR: {
        if (f->max_items == 0) return SIZE_MAX;
        size_t eb = bb_json_bound_elem(f, depth);
        if (eb == SIZE_MAX) return SIZE_MAX;
        return 2 + (size_t)f->max_items * (eb + 1);
    }
    default:
        return 0;  // LCOV_EXCL_LINE -- exhaustive enum, defensive
    }
}

// Worst-case bytes for one field, key included (`"key":value,`).
static size_t bb_json_bound_field(const bb_serialize_field_t *f, unsigned depth)
{
    size_t vmax = bb_json_bound_value(f, depth);
    if (vmax == SIZE_MAX) return SIZE_MAX;
    size_t key_cost = 6 * strlen(f->key) + 3;
    return key_cost + vmax + 1;
}

static size_t bb_json_bound_fields(const bb_serialize_field_t *fields, uint16_t n_fields,
                                    unsigned depth)
{
    size_t total = 0;
    for (uint16_t i = 0; i < n_fields; i++) {
        size_t c = bb_json_bound_field(&fields[i], depth);
        if (c == SIZE_MAX) return SIZE_MAX;
        total += c;
    }
    return total;
}

size_t bb_serialize_json_bound(const bb_serialize_desc_t *desc)
{
    if (!desc) return 0;  // defensive against NULL misuse

    size_t total = bb_json_bound_fields(desc->fields, desc->n_fields, 0);
    if (total == SIZE_MAX) return SIZE_MAX;
    return 2 + total + 1;
}
