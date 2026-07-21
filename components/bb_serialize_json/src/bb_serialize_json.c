// Hand-rolled, no-heap, bounded-buffer JSON bb_serialize_emit_t backend.
// All writes funnel through bb_json_put()/bb_json_putc() so a single
// capacity check (in bb_json_put) governs every byte written -- overflow
// sets a sticky error and stops copying immediately, never a partial write.

#include "bb_serialize_json.h"

#include "bb_num.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

// ---------------------------------------------------------------------------
// Kconfig bridge -- CONFIG_BB_SERIALIZE_JSON_STREAM_FLUSH_BUF_BYTES -> a C
// default. Never shadow the generated symbol with a bare #ifndef.
// ---------------------------------------------------------------------------
#ifdef CONFIG_BB_SERIALIZE_JSON_STREAM_FLUSH_BUF_BYTES
#define BB_SERIALIZE_JSON_STREAM_FLUSH_BUF_BYTES CONFIG_BB_SERIALIZE_JSON_STREAM_FLUSH_BUF_BYTES
#else
#define BB_SERIALIZE_JSON_STREAM_FLUSH_BUF_BYTES 768
#endif

// Build-enforced counterpart to the "no single walker-driven put() exceeds
// the flush buffer" invariant bb_json_put()'s `n > ctx->cap` branch below
// relies on being unreachable (see its LCOV_EXCL_LINE comment). The largest
// single put() call any descriptor-driven emit path can make is a u64/i64
// digit string (bb_json_write_u64/_i64: at most 21 bytes, sign + 20 decimal
// digits) or a single \uXXXX control-char escape unit (bb_json_escape_write:
// 6 bytes) -- 32 is a safe floor well above either. As long as the flush
// buffer stays >= this floor, that branch is provably dead for any walker-
// driven caller; a Kconfig value below the floor would make it reachable,
// so this is checked at compile time rather than left as a comment-only
// invariant.
_Static_assert(BB_SERIALIZE_JSON_STREAM_FLUSH_BUF_BYTES >= 32,
               "BB_SERIALIZE_JSON_STREAM_FLUSH_BUF_BYTES must be >= 32 -- "
               "smaller than the largest single walker-driven put() call "
               "(a 21-byte u64/i64 digit string), which would make the "
               "excluded bb_json_put() n > ctx->cap branch reachable");

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

// Flushes whatever's currently buffered via ctx->flush_fn (a no-op if
// unbound, already poisoned, or the buffer is empty). Polls
// ctx->flush_failed after the flush call and, if the caller has signaled
// abort, sets the sticky synthetic BB_ERR_INVALID_STATE so every subsequent
// bb_json_put() short-circuits (no further flush_fn calls) -- see
// bb_serialize_json_stream_render()'s doc comment for why this specific
// code is reused as the abort signal.
static void bb_json_flush(bb_serialize_json_ctx_t *ctx)
{
    if (ctx->err != BB_OK || !ctx->flush_fn || ctx->len == 0) return;
    ctx->flush_fn(ctx->flush_ctx, ctx->buf, ctx->len);
    ctx->len = 0;
    if (ctx->flush_failed && *ctx->flush_failed) ctx->err = BB_ERR_INVALID_STATE;  // sticky abort
}

static void bb_json_put(bb_serialize_json_ctx_t *ctx, const char *s, size_t n)
{
    if (ctx->err != BB_OK) return;
    if (ctx->len + n > ctx->cap) {
        if (!ctx->flush_fn) {
            ctx->err = BB_ERR_NO_SPACE;  // unchanged bounded all-or-nothing behavior
            return;
        }
        bb_json_flush(ctx);
        if (ctx->err != BB_OK) return;
        // Defensive: no walker-driven single put() call (max key/number/
        // literal/escape-unit width) can ever exceed the Kconfig-floored
        // 128-byte-minimum flush buffer -- untestable without a hand-rolled
        // emit callback bypassing the walker entirely.
        if (n > ctx->cap) {
            ctx->err = BB_ERR_NO_SPACE;  // LCOV_EXCL_LINE -- single write larger than the whole flush buffer
            return;  // LCOV_EXCL_LINE
        }
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
// Number formatting -- portable u64/i64 -> decimal delegated to bb_num (no
// snprintf, no locale, no libc `ll`-format dependency); the JSON-specific
// bit is funneling the result through bb_json_put() so the sticky
// all-or-nothing overflow contract still governs every byte written.
// ---------------------------------------------------------------------------

static void bb_json_write_u64(bb_serialize_json_ctx_t *ctx, uint64_t v)
{
    char   digits[21];  // 20 decimal digits + NUL
    size_t n = bb_num_u64_to_dec(digits, sizeof(digits), v);
    bb_json_put(ctx, digits, n);
}

static void bb_json_write_i64(bb_serialize_json_ctx_t *ctx, int64_t v)
{
    char   digits[22];  // sign + 20 decimal digits + NUL
    size_t n = bb_num_i64_to_dec(digits, sizeof(digits), v);
    bb_json_put(ctx, digits, n);
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

// Shortest-round-trippable double formatting, BYTE-IDENTICAL to cJSON's
// internal print_number() (verified against cJSON v1.7.18's actual source,
// not assumed): NaN/Inf emit `null` (same convention as bb_json_write_f64()
// above).
//
// INT32 FAST PATH -- cJSON's print_number() does NOT go straight to
// "%.15g"/"%.17g": at set-number time it caches a saturating (int)cast of
// the double (INT32_MAX/INT32_MIN if out of range, else (int)v) as
// item->valueint, then AT PRINT TIME, whenever `v == (double)valueint`,
// prints "%d" of THAT INT -- never touching %g at all. This must be
// reproduced exactly, not just approximated by %g's own whole-number
// formatting, because an int has no negative-zero representation: -0.0
// takes this fast path (valueint == 0, and -0.0 == (double)0 is true under
// IEEE 754 signed-zero equality) and prints "0", NOT "-0" -- verified by
// direct differential test against cJSON, not assumed (see
// test_bb_serialize_json_f64_shortest_negative_zero). Every other
// whole-integer double in range (e.g. 3.0 -> "3", -5.0 -> "-5") also goes
// through this path and happens to agree with what %g would print anyway
// (no decimal point), EXCEPT at the -0.0 sign-loss case above.
//
// Otherwise (v not exactly its own int32 cast): `snprintf(..., "%.15g", v)`,
// then a round-trip check via strtod() (per this PR's design -- not
// sscanf+cJSON's own fuzzy compare_double() relative-epsilon compare; an
// EXACT strtod()==v check is a strictly stricter round-trip test, so it can
// only ever choose %.17g in cases cJSON's looser check would still accept
// %.15g for -- a theoretical, exceedingly narrow divergence band no swept
// value here falls into) -- on strtod mismatch, re-render at `%.17g`.
//
// Opt-in via ctx->f64_shortest (see bb_json_cb_emit_f64 below); default
// (false) leaves every existing caller on bb_json_write_f64() unchanged.
static void bb_json_write_f64_shortest(bb_serialize_json_ctx_t *ctx, double v)
{
    if (isnan(v) || isinf(v)) {
        bb_json_put(ctx, "null", 4);
        return;
    }

    int32_t vi;
    if (v >= (double)INT32_MAX) {
        vi = INT32_MAX;
    } else if (v <= (double)INT32_MIN) {
        vi = INT32_MIN;
    } else {
        vi = (int32_t)v;
    }

    char buf[32];
    int  n;
    if (v == (double)vi) {
        n = snprintf(buf, sizeof(buf), "%d", vi);
    } else {
        n = snprintf(buf, sizeof(buf), "%.15g", v);
        double reparse = strtod(buf, NULL);
        double max_abs = fabs(reparse) > fabs(v) ? fabs(reparse) : fabs(v);
        // Match cJSON's compare_double() exactly (relative-epsilon accept,
        // not exact ==) so byte output stays identical to cJSON's own
        // print_number() -- see cJSON.c compare_double()/print_number().
        if (fabs(reparse - v) > max_abs * DBL_EPSILON) {
            n = snprintf(buf, sizeof(buf), "%.17g", v);
        }
    }
    bb_json_put(ctx, buf, (size_t)n);
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

// bb_serialize_json_register_format() registers a ctx-less template vtable
// (ctx == NULL) into the format-dispatch registry -- a lookup caller MUST
// copy the template and bind a real ctx before walking it (see
// bb_serialize_format.h). vctx == NULL here means an unbound template was
// walked directly, a composition bug upstream (fail loud, never NULL-deref
// into ctx->depth/ctx->stack below).
static bb_serialize_json_ctx_t *bb_json_cb_ctx(void *vctx)
{
    assert(vctx != NULL && "bb_serialize_json emit callback invoked with NULL ctx -- unbound template walked directly");  // LCOV_EXCL_BR_LINE -- abort branch untestable, no death-test harness
    return (bb_serialize_json_ctx_t *)vctx;
}

static void bb_json_cb_begin_obj(void *vctx, const char *key)
{
    bb_serialize_json_ctx_t *ctx = bb_json_cb_ctx(vctx);
    bb_json_pre_value(ctx, key);
    bb_json_putc(ctx, '{');
    bb_json_push_level(ctx, false);
}

static void bb_json_cb_end_obj(void *vctx)
{
    bb_serialize_json_ctx_t *ctx = bb_json_cb_ctx(vctx);
    bb_json_putc(ctx, '}');
    bb_json_pop_level(ctx);
}

static void bb_json_cb_begin_arr(void *vctx, const char *key)
{
    bb_serialize_json_ctx_t *ctx = bb_json_cb_ctx(vctx);
    bb_json_pre_value(ctx, key);
    bb_json_putc(ctx, '[');
    bb_json_push_level(ctx, true);
}

static void bb_json_cb_end_arr(void *vctx)
{
    bb_serialize_json_ctx_t *ctx = bb_json_cb_ctx(vctx);
    bb_json_putc(ctx, ']');
    bb_json_pop_level(ctx);
}

static void bb_json_cb_emit_i64(void *vctx, const char *key, int64_t v)
{
    bb_serialize_json_ctx_t *ctx = bb_json_cb_ctx(vctx);
    bb_json_pre_value(ctx, key);
    bb_json_write_i64(ctx, v);
}

static void bb_json_cb_emit_u64(void *vctx, const char *key, uint64_t v)
{
    bb_serialize_json_ctx_t *ctx = bb_json_cb_ctx(vctx);
    bb_json_pre_value(ctx, key);
    bb_json_write_u64(ctx, v);
}

static void bb_json_cb_emit_f64(void *vctx, const char *key, double v)
{
    bb_serialize_json_ctx_t *ctx = bb_json_cb_ctx(vctx);
    bb_json_pre_value(ctx, key);
    if (ctx->f64_shortest) {
        bb_json_write_f64_shortest(ctx, v);
    } else {
        bb_json_write_f64(ctx, v);
    }
}

static void bb_json_cb_emit_bool(void *vctx, const char *key, bool v)
{
    bb_serialize_json_ctx_t *ctx = bb_json_cb_ctx(vctx);
    bb_json_pre_value(ctx, key);
    if (v) {
        bb_json_put(ctx, "true", 4);
    } else {
        bb_json_put(ctx, "false", 5);
    }
}

static void bb_json_cb_emit_str(void *vctx, const char *key, const char *s, size_t len)
{
    bb_serialize_json_ctx_t *ctx = bb_json_cb_ctx(vctx);
    bb_json_pre_value(ctx, key);
    bb_json_putc(ctx, '"');
    bb_json_escape_write(ctx, s, len);
    bb_json_putc(ctx, '"');
}

static void bb_json_cb_emit_null(void *vctx, const char *key)
{
    bb_serialize_json_ctx_t *ctx = bb_json_cb_ctx(vctx);
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
    ctx->flush_fn = NULL;
    ctx->flush_ctx = NULL;
    ctx->flush_failed = NULL;
    ctx->f64_shortest = false;  // default: today's fixed-decimal f64 formatting, unchanged
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

// Shared implementation -- resolve == NULL drives the plain
// bb_serialize_walk() path (via bb_serialize_walk_ref's own NULL handling).
// f64_shortest selects bb_json_write_f64_shortest() over the default
// bb_json_write_f64() for every BB_TYPE_F64 field this call renders (see
// bb_serialize_json_ctx_t.f64_shortest's doc comment) -- a render-level
// choice, not per-field.
static bb_err_t bb_json_render_impl(const bb_serialize_desc_t *desc, const void *snap,
                                     char *buf, size_t cap, size_t *out_len,
                                     bb_serialize_ref_resolve_fn resolve, void *resolve_ctx,
                                     bool f64_shortest)
{
    if (cap == 0) return BB_ERR_NO_SPACE;

    // Reserve the final byte for the NUL terminator -- the writer itself
    // never accounts for it.
    bb_serialize_json_ctx_t ctx;
    bb_serialize_json_ctx_init(&ctx, buf, cap - 1);
    ctx.f64_shortest = f64_shortest;

    bb_serialize_emit_t emit = bb_serialize_json_emit(&ctx);

    bb_json_putc(&ctx, '{');
    bb_serialize_walk_ref(desc, snap, &emit, resolve, resolve_ctx);
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

// Thin wrapper: bb_serialize_json_render_ex(desc, snap, buf, cap, out_len,
// false) -- same "_ex adds a trailing opt-in param, original keeps today's
// default" idiom as e.g. bb_queue_create_ex() (bb_cache took the other
// approach for its own opt-in first-time reporting: a nullable
// cfg->out_first_time field folded into bb_cache_register() itself, no _ex
// variant -- see bb_cache.h, B1-1118).
bb_err_t bb_serialize_json_render(const bb_serialize_desc_t *desc, const void *snap,
                                   char *buf, size_t cap, size_t *out_len)
{
    return bb_serialize_json_render_ex(desc, snap, buf, cap, out_len, false);
}

bb_err_t bb_serialize_json_render_ex(const bb_serialize_desc_t *desc, const void *snap,
                                      char *buf, size_t cap, size_t *out_len,
                                      bool f64_shortest)
{
    return bb_json_render_impl(desc, snap, buf, cap, out_len, NULL, NULL, f64_shortest);
}

// Thin wrapper: bb_serialize_json_render_ref_ex(..., false) -- see
// bb_serialize_json_render()'s doc comment above for the idiom.
bb_err_t bb_serialize_json_render_ref(const bb_serialize_desc_t *desc, const void *snap,
                                       char *buf, size_t cap, size_t *out_len,
                                       bb_serialize_ref_resolve_fn resolve, void *resolve_ctx)
{
    return bb_serialize_json_render_ref_ex(desc, snap, buf, cap, out_len, resolve, resolve_ctx, false);
}

bb_err_t bb_serialize_json_render_ref_ex(const bb_serialize_desc_t *desc, const void *snap,
                                          char *buf, size_t cap, size_t *out_len,
                                          bb_serialize_ref_resolve_fn resolve, void *resolve_ctx,
                                          bool f64_shortest)
{
    return bb_json_render_impl(desc, snap, buf, cap, out_len, resolve, resolve_ctx, f64_shortest);
}

// Distinct entry point from bb_json_render_impl() above -- NOT routed
// through bb_serialize_format_render() (that dispatch stays bounded/
// unchanged). Drives the walker against a small internal stack buffer,
// flushing through `flush_fn` whenever it fills, so an arbitrarily large
// document renders through a small, bounded working set. See the header's
// doc comment for the abort/error-code contract.
bb_err_t bb_serialize_json_stream_render(const bb_serialize_desc_t *desc, const void *snap,
                                          bb_serialize_json_flush_fn flush_fn, void *flush_ctx,
                                          const volatile bool *flush_failed)
{
    return bb_serialize_json_stream_render_ex(desc, snap, flush_fn, flush_ctx, flush_failed, false);
}

// bb_serialize_json_stream_render() is a thin wrapper: this fn with
// f64_shortest == false. See bb_json_render_impl()'s doc comment above for
// the f64_shortest contract.
bb_err_t bb_serialize_json_stream_render_ex(const bb_serialize_desc_t *desc, const void *snap,
                                             bb_serialize_json_flush_fn flush_fn, void *flush_ctx,
                                             const volatile bool *flush_failed, bool f64_shortest)
{
    char buf[BB_SERIALIZE_JSON_STREAM_FLUSH_BUF_BYTES];
    bb_serialize_json_ctx_t ctx;
    bb_serialize_json_ctx_init(&ctx, buf, sizeof(buf));
    ctx.flush_fn = flush_fn;
    ctx.flush_ctx = flush_ctx;
    ctx.flush_failed = flush_failed;
    ctx.f64_shortest = f64_shortest;

    bb_serialize_emit_t emit = bb_serialize_json_emit(&ctx);

    bb_json_putc(&ctx, '{');
    bb_serialize_walk_ref(desc, snap, &emit, NULL, NULL);
    bb_json_putc(&ctx, '}');
    bb_json_flush(&ctx);  // flush the tail

    return ctx.err;
}

// Composed-document counterpart -- same buffer, same flush/abort mechanics
// as bb_serialize_json_stream_render() above, driving one
// bb_serialize_compose_walk() call per group (rather than a single
// bb_serialize_walk_ref() call) inside one pair of root braces -- letting a
// caller mix shapes (e.g. RAW root fields + OBJECT sections) that no single
// bb_serialize_compose_walk() call can express on its own. See the header's
// doc comment for the mixed-shape rationale and the compose_err/ctx.err
// precedence contract.
bb_err_t bb_serialize_json_stream_compose_render(const bb_serialize_compose_group_t *groups, size_t n_groups,
                                                  bb_serialize_json_flush_fn flush_fn, void *flush_ctx,
                                                  const volatile bool *flush_failed)
{
    return bb_serialize_json_stream_compose_render_ex(groups, n_groups, flush_fn, flush_ctx, flush_failed, false);
}

// bb_serialize_json_stream_compose_render() is a thin wrapper: this fn with
// f64_shortest == false. See bb_json_render_impl()'s doc comment above for
// the f64_shortest contract.
bb_err_t bb_serialize_json_stream_compose_render_ex(const bb_serialize_compose_group_t *groups, size_t n_groups,
                                                     bb_serialize_json_flush_fn flush_fn, void *flush_ctx,
                                                     const volatile bool *flush_failed, bool f64_shortest)
{
    char buf[BB_SERIALIZE_JSON_STREAM_FLUSH_BUF_BYTES];
    bb_serialize_json_ctx_t ctx;
    bb_serialize_json_ctx_init(&ctx, buf, sizeof(buf));
    ctx.flush_fn = flush_fn;
    ctx.flush_ctx = flush_ctx;
    ctx.flush_failed = flush_failed;
    ctx.f64_shortest = f64_shortest;

    bb_serialize_emit_t emit = bb_serialize_json_emit(&ctx);

    bb_json_putc(&ctx, '{');
    bb_err_t compose_err = BB_OK;
    for (size_t i = 0; i < n_groups; i++) {
        const bb_serialize_compose_group_t *g = &groups[i];
        compose_err = bb_serialize_compose_walk(g->entries, g->n, g->shape, &emit);
        if (compose_err != BB_OK) break;
    }
    bb_json_putc(&ctx, '}');
    bb_json_flush(&ctx);  // flush the tail

    if (compose_err != BB_OK) return compose_err;
    return ctx.err;
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
        // BB_ARR_STREAM's row count isn't known statically -- unbounded,
        // same convention as max_items == 0 below (which every STREAM field
        // also naturally has, since it carries no destination capacity).
        if (f->cardinality == BB_ARR_STREAM) return SIZE_MAX;
        if (f->max_items == 0) return SIZE_MAX;
        size_t eb = bb_json_bound_elem(f, depth);
        if (eb == SIZE_MAX) return SIZE_MAX;
        return 2 + (size_t)f->max_items * (eb + 1);
    }
    case BB_TYPE_REF:
        // A REF's rendered size depends on the resolver's sibling
        // descriptor, which isn't known statically here -- same unbounded
        // convention as max_len==0 / max_items==0 above.
        return SIZE_MAX;
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
