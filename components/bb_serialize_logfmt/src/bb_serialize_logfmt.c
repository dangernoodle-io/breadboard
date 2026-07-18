// Hand-rolled, no-heap, bounded-buffer logfmt bb_serialize_emit_t backend.
// All writes funnel through bb_logfmt_put()/bb_logfmt_putc() so a single
// capacity check (in bb_logfmt_put) governs every byte written -- overflow
// sets a sticky error and stops copying immediately, never a partial write.
// Mirrors bb_serialize_json.c's structure; the logfmt-specific bit is value
// quoting/escaping instead of JSON string/container syntax.

#include "bb_serialize_logfmt.h"

#include "bb_num.h"

#include <assert.h>
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Kconfig bridge -- CONFIG_BB_SERIALIZE_LOGFMT_F64_DECIMALS -> a C default.
// Never shadow the generated symbol with a bare #ifndef.
// ---------------------------------------------------------------------------
#ifdef CONFIG_BB_SERIALIZE_LOGFMT_F64_DECIMALS
#define BB_SERIALIZE_LOGFMT_F64_DECIMALS CONFIG_BB_SERIALIZE_LOGFMT_F64_DECIMALS
#else
#define BB_SERIALIZE_LOGFMT_F64_DECIMALS 6
#endif

// 10^N table, N in [0, 15] -- covers the Kconfig knob's full range.
static const double s_pow10[16] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7,
    1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
};

// 2^64 -- the boundary above which a double's integer part no longer fits
// a uint64_t (used as the f64 documented-range-limit check).
#define BB_LOGFMT_TWO_POW_64 18446744073709551616.0

// ---------------------------------------------------------------------------
// Low-level bounded writers -- every byte written funnels through these.
// ---------------------------------------------------------------------------

static void bb_logfmt_put(bb_serialize_logfmt_ctx_t *ctx, const char *s, size_t n)
{
    if (ctx->err != BB_OK) return;
    if (ctx->len + n > ctx->cap) {
        ctx->err = BB_ERR_NO_SPACE;
        return;
    }
    memcpy(ctx->buf + ctx->len, s, n);
    ctx->len += n;
}

static void bb_logfmt_putc(bb_serialize_logfmt_ctx_t *ctx, char ch)
{
    bb_logfmt_put(ctx, &ch, 1);
}

// ---------------------------------------------------------------------------
// Value quoting -- the logfmt-specific bit (as popularized by
// github.com/go-logfmt/logfmt): a value is quoted+escaped if it is empty, or
// contains a space, `"`, `=`, `\`, or any control byte (< 0x20). An unquoted
// value therefore never needs escaping -- callers scanning a rendered line
// with a simple "split on space, split on first =" parser never see a raw
// space/quote/= inside an unquoted token.
// ---------------------------------------------------------------------------

static bool bb_logfmt_needs_quote(const char *s, size_t len)
{
    if (len == 0) return true;  // distinguish empty from "no value at all"

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '"' || c == '=' || c == '\\' || c < 0x20) return true;
    }
    return false;
}

// Writes (s, len) wrapped in double quotes, escaping `"` -> `\"`, `\` ->
// `\\`, and the common control bytes (\n \r \t) as their two-character
// escape; any other control byte (< 0x20) is dropped down to a `\xHH`
// escape. Each escape sequence is written by a single put() call so overflow
// can never leave a half-written escape.
static void bb_logfmt_write_quoted(bb_serialize_logfmt_ctx_t *ctx, const char *s, size_t len)
{
    bb_logfmt_putc(ctx, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  bb_logfmt_put(ctx, "\\\"", 2); break;
        case '\\': bb_logfmt_put(ctx, "\\\\", 2); break;
        case '\n': bb_logfmt_put(ctx, "\\n", 2); break;
        case '\r': bb_logfmt_put(ctx, "\\r", 2); break;
        case '\t': bb_logfmt_put(ctx, "\\t", 2); break;
        default:
            if (c < 0x20) {
                static const char hex[] = "0123456789abcdef";
                char esc[4] = { '\\', 'x', hex[(c >> 4) & 0xFU], hex[c & 0xFU] };
                bb_logfmt_put(ctx, esc, sizeof(esc));
            } else {
                bb_logfmt_putc(ctx, (char)c);
            }
            break;
        }
    }
    bb_logfmt_putc(ctx, '"');
}

// Writes (s, len) as a logfmt value: quoted+escaped if bb_logfmt_needs_quote()
// says so, otherwise written verbatim (no escaping needed by construction).
static void bb_logfmt_write_str(bb_serialize_logfmt_ctx_t *ctx, const char *s, size_t len)
{
    if (bb_logfmt_needs_quote(s, len)) {
        bb_logfmt_write_quoted(ctx, s, len);
    } else {
        bb_logfmt_put(ctx, s, len);
    }
}

// ---------------------------------------------------------------------------
// Number formatting -- portable u64/i64 -> decimal delegated to bb_num (no
// snprintf, no locale, no libc `ll`-format dependency); the logfmt-specific
// bit is funneling the result through bb_logfmt_put() so the sticky
// all-or-nothing overflow contract still governs every byte written.
// ---------------------------------------------------------------------------

static void bb_logfmt_write_u64(bb_serialize_logfmt_ctx_t *ctx, uint64_t v)
{
    char   digits[21];  // 20 decimal digits + NUL
    size_t n = bb_num_u64_to_dec(digits, sizeof(digits), v);
    bb_logfmt_put(ctx, digits, n);
}

static void bb_logfmt_write_i64(bb_serialize_logfmt_ctx_t *ctx, int64_t v)
{
    char   digits[22];  // sign + 20 decimal digits + NUL
    size_t n = bb_num_i64_to_dec(digits, sizeof(digits), v);
    bb_logfmt_put(ctx, digits, n);
}

// Fixed-width zero-padded fractional digit write (single put() call).
static void bb_logfmt_write_frac_digits(bb_serialize_logfmt_ctx_t *ctx, uint64_t frac, int n_digits)
{
    char out[16];
    for (int i = n_digits - 1; i >= 0; i--) {
        out[i] = (char)('0' + (frac % 10));
        frac /= 10;
    }
    bb_logfmt_put(ctx, out, (size_t)n_digits);
}

// Deterministic fixed-decimal double formatting -- same algorithm as
// bb_serialize_json.c's bb_json_write_f64(): N fractional digits
// (BB_SERIALIZE_LOGFMT_F64_DECIMALS), round-to-nearest via scale-and-round,
// with the rounding carry propagated into the integer part when it
// overflows the fractional scale. NaN/Inf and magnitudes whose integer part
// exceeds uint64_t range emit `null` (unquoted, matching this backend's
// emit_null literal) -- a documented limit, not a sticky error.
static void bb_logfmt_write_f64(bb_serialize_logfmt_ctx_t *ctx, double v)
{
    if (isnan(v) || isinf(v)) {
        bb_logfmt_put(ctx, "null", 4);
        return;
    }

    bool   neg = v < 0.0;
    double mag = neg ? -v : v;

    if (mag >= BB_LOGFMT_TWO_POW_64) {
        bb_logfmt_put(ctx, "null", 4);
        return;
    }

    uint64_t int_part = (uint64_t)mag;
    double   frac = mag - (double)int_part;

    double   scale = s_pow10[BB_SERIALIZE_LOGFMT_F64_DECIMALS];
    uint64_t scale_i = (uint64_t)scale;
    uint64_t frac_i = (uint64_t)(frac * scale + 0.5);
    if (frac_i >= scale_i) {
        frac_i -= scale_i;
        int_part += 1;
    }

    if (neg) bb_logfmt_putc(ctx, '-');
    bb_logfmt_write_u64(ctx, int_part);
    bb_logfmt_putc(ctx, '.');
    bb_logfmt_write_frac_digits(ctx, frac_i, BB_SERIALIZE_LOGFMT_F64_DECIMALS);
}

// ---------------------------------------------------------------------------
// Separator / key-prefix bookkeeping -- flat "key=value key=value" line, no
// container brackets (nested OBJ/ARR are structurally safe no-ops, see the
// file header comment / bb_serialize_logfmt_emit()'s doc comment).
// ---------------------------------------------------------------------------

static void bb_logfmt_pre_value(bb_serialize_logfmt_ctx_t *ctx, const char *key)
{
    if (ctx->len > 0) bb_logfmt_putc(ctx, ' ');
    if (key) {
        bb_logfmt_put(ctx, key, strlen(key));
        bb_logfmt_putc(ctx, '=');
    }
}

// ---------------------------------------------------------------------------
// bb_serialize_emit_t callbacks
// ---------------------------------------------------------------------------

// bb_serialize_logfmt_register_format() registers a ctx-less template vtable
// (ctx == NULL) into the format-dispatch registry -- a lookup caller MUST
// copy the template and bind a real ctx before walking it (see
// bb_serialize_format.h). vctx == NULL here means an unbound template was
// walked directly, a composition bug upstream (fail loud, never NULL-deref
// into ctx->len/ctx->cap below).
static bb_serialize_logfmt_ctx_t *bb_logfmt_cb_ctx(void *vctx)
{
    assert(vctx != NULL && "bb_serialize_logfmt emit callback invoked with NULL ctx -- unbound template walked directly");  // LCOV_EXCL_BR_LINE -- abort branch untestable, no death-test harness
    return (bb_serialize_logfmt_ctx_t *)vctx;
}

// Nested containers are structurally supported (never crash) but this
// backend is designed for flat, scalar-only descriptors -- see
// bb_serialize_logfmt_emit()'s doc comment. begin/end are deliberate
// no-ops: no bracket characters, no key-prefix bookkeeping.
static void bb_logfmt_cb_begin_obj(void *vctx, const char *key)
{
    (void)bb_logfmt_cb_ctx(vctx);
    (void)key;
}

static void bb_logfmt_cb_end_obj(void *vctx)
{
    (void)bb_logfmt_cb_ctx(vctx);
}

static void bb_logfmt_cb_begin_arr(void *vctx, const char *key)
{
    (void)bb_logfmt_cb_ctx(vctx);
    (void)key;
}

static void bb_logfmt_cb_end_arr(void *vctx)
{
    (void)bb_logfmt_cb_ctx(vctx);
}

static void bb_logfmt_cb_emit_i64(void *vctx, const char *key, int64_t v)
{
    bb_serialize_logfmt_ctx_t *ctx = bb_logfmt_cb_ctx(vctx);
    bb_logfmt_pre_value(ctx, key);
    bb_logfmt_write_i64(ctx, v);
}

static void bb_logfmt_cb_emit_u64(void *vctx, const char *key, uint64_t v)
{
    bb_serialize_logfmt_ctx_t *ctx = bb_logfmt_cb_ctx(vctx);
    bb_logfmt_pre_value(ctx, key);
    bb_logfmt_write_u64(ctx, v);
}

static void bb_logfmt_cb_emit_f64(void *vctx, const char *key, double v)
{
    bb_serialize_logfmt_ctx_t *ctx = bb_logfmt_cb_ctx(vctx);
    bb_logfmt_pre_value(ctx, key);
    bb_logfmt_write_f64(ctx, v);
}

static void bb_logfmt_cb_emit_bool(void *vctx, const char *key, bool v)
{
    bb_serialize_logfmt_ctx_t *ctx = bb_logfmt_cb_ctx(vctx);
    bb_logfmt_pre_value(ctx, key);
    if (v) {
        bb_logfmt_put(ctx, "true", 4);
    } else {
        bb_logfmt_put(ctx, "false", 5);
    }
}

static void bb_logfmt_cb_emit_str(void *vctx, const char *key, const char *s, size_t len)
{
    bb_serialize_logfmt_ctx_t *ctx = bb_logfmt_cb_ctx(vctx);
    bb_logfmt_pre_value(ctx, key);
    bb_logfmt_write_str(ctx, s, len);
}

static void bb_logfmt_cb_emit_null(void *vctx, const char *key)
{
    bb_serialize_logfmt_ctx_t *ctx = bb_logfmt_cb_ctx(vctx);
    bb_logfmt_pre_value(ctx, key);
    bb_logfmt_put(ctx, "null", 4);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void bb_serialize_logfmt_ctx_init(bb_serialize_logfmt_ctx_t *ctx, char *buf, size_t cap)
{
    ctx->buf = buf;
    ctx->cap = cap;
    ctx->len = 0;
    ctx->err = BB_OK;
}

bb_serialize_emit_t bb_serialize_logfmt_emit(bb_serialize_logfmt_ctx_t *ctx)
{
    bb_serialize_emit_t emit = {
        .format_id = BB_FORMAT_LOGFMT,
        .ctx = ctx,
        .begin_obj = bb_logfmt_cb_begin_obj,
        .end_obj = bb_logfmt_cb_end_obj,
        .begin_arr = bb_logfmt_cb_begin_arr,
        .end_arr = bb_logfmt_cb_end_arr,
        .emit_i64 = bb_logfmt_cb_emit_i64,
        .emit_u64 = bb_logfmt_cb_emit_u64,
        .emit_f64 = bb_logfmt_cb_emit_f64,
        .emit_bool = bb_logfmt_cb_emit_bool,
        .emit_str = bb_logfmt_cb_emit_str,
        .emit_null = bb_logfmt_cb_emit_null,
    };
    return emit;
}

bb_err_t bb_serialize_logfmt_render(const bb_serialize_desc_t *desc, const void *snap,
                                     char *buf, size_t cap, size_t *out_len)
{
    if (!buf || cap == 0) {
        if (out_len) *out_len = 0;
        return BB_ERR_NO_SPACE;
    }

    // Reserve the final byte for the NUL terminator -- the writer itself
    // never accounts for it.
    bb_serialize_logfmt_ctx_t ctx;
    bb_serialize_logfmt_ctx_init(&ctx, buf, cap - 1);

    bb_serialize_emit_t emit = bb_serialize_logfmt_emit(&ctx);
    bb_serialize_walk(desc, snap, &emit);

    if (ctx.err != BB_OK) {
        buf[0] = '\0';
        if (out_len) *out_len = 0;
        return ctx.err;
    }

    buf[ctx.len] = '\0';
    if (out_len) *out_len = ctx.len;
    return BB_OK;
}
