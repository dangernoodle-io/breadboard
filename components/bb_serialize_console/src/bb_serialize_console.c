// Hand-rolled, no-heap, bounded-buffer human-readable "key=val key=val" line
// backend -- the bb_serialize_console counterpart to bb_serialize_json.
// Every write funnels through bb_console_appendf(), which uses vsnprintf's
// own truncation semantics: on overflow, the write clips at the buffer
// boundary and the buffer stays NUL-terminated -- no sticky all-or-nothing
// error like the JSON backend's bb_json_put(). A clipped heap line logged
// over serial is still useful.

#include "bb_serialize_console.h"

#include "bb_num.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Low-level bounded writer -- every byte written funnels through this.
// ---------------------------------------------------------------------------

// Appends a printf-formatted fragment to `ctx`, clipped to remaining
// capacity. Always leaves `ctx->buf` NUL-terminated within `ctx->cap` (a
// property vsnprintf itself guarantees for any size > 0). A no-op if there
// is no room left even for the NUL terminator.
static void bb_console_appendf(bb_serialize_console_ctx_t *ctx, const char *fmt, ...)
{
    // Reachable: every prior append via this file's own call sites leaves
    // ctx->len <= ctx->cap - 1 (see the `written = avail - 1` clamp below),
    // but a caller-constructed zero-capacity ctx (bb_serialize_console_ctx_
    // init(&ctx, buf, 0), bypassing bb_serialize_console_render()'s own
    // cap == 0 guard) starts with ctx->len == ctx->cap == 0, hitting this
    // guard's true arm on the very first append.
    if (ctx->len + 1 > ctx->cap) return;

    size_t avail = ctx->cap - ctx->len;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(ctx->buf + ctx->len, avail, fmt, ap);
    va_end(ap);

    if (n <= 0) return;  // encoding error, or nothing to append

    size_t written = (size_t)n;
    if (written >= avail) written = avail - 1;  // clipped; still NUL-terminated
    ctx->len += written;
}

// Writes the leading space separator (if a prior token exists) and, for a
// keyed (non-array-element) value, the "key=" prefix -- qualified with the
// current path stack (B1-1416), e.g. "internal.free=" for a field keyed
// "free" nested inside an object keyed "internal". Ancestor path entries
// that were dropped by bb_console_push_path()'s depth-cap guard (or that
// are NULL, e.g. an array-of-obj element with no key of its own) are
// skipped rather than joined as an empty segment.
static void bb_console_pre_value(bb_serialize_console_ctx_t *ctx, const char *key)
{
    if (ctx->len > 0) bb_console_appendf(ctx, " ");
    if (!key) return;

    for (size_t i = 0; i < ctx->path_depth; i++) {
        if (ctx->path[i]) bb_console_appendf(ctx, "%s.", ctx->path[i]);
    }
    bb_console_appendf(ctx, "%s=", key);
}

// Pushes `key` onto the path stack for the duration of the enclosing
// BB_TYPE_OBJ. Defensive against exceeding BB_SERIALIZE_MAX_DEPTH: the
// walker itself refuses to recurse past that cap (see bb_serialize_walk.c),
// so a legitimate walk can never reach the guard's true arm, but this
// backend truncates rather than fails on any overflow, so a would-be
// overflow here silently drops that level's qualification instead of
// corrupting the array. `key` may be NULL (an array-of-obj element has no
// key of its own); the stack still advances so end_obj's pop stays
// balanced with begin_obj's push.
static void bb_console_push_path(bb_serialize_console_ctx_t *ctx, const char *key)
{
    if (ctx->path_depth >= BB_SERIALIZE_MAX_DEPTH) return;  // LCOV_EXCL_LINE -- unreachable via bb_serialize_walk(), which caps depth itself
    ctx->path[ctx->path_depth++] = key;
}

// Symmetric with bb_console_push_path()'s guard: today the two invariants
// that make push-skip-on-cap and unconditional pop line up (the walker
// never recurses past BB_SERIALIZE_MAX_DEPTH before a begin_obj, and it
// never early-returns between a begin_obj and its matching end_obj) are
// enforced nowhere but bb_serialize_walk.c itself -- a future walker change
// that breaks either one desyncs path_depth from the real nesting silently:
// no crash, no error return, just every subsequent sibling key rendering
// with the wrong prefix. assert() converts that into a loud, testable
// failure the moment it happens (B1-1416); the `if` beneath it stays as an
// unconditional safety net for NDEBUG builds, since path_depth is a size_t
// and decrementing it past 0 would wrap to SIZE_MAX rather than merely
// under-report -- bb_console_push_path()'s own cap check would then treat
// every subsequent push as "at cap" and silently drop qualification for the
// rest of the process's lifetime instead of just this one render.
static void bb_console_pop_path(bb_serialize_console_ctx_t *ctx)
{
    assert(ctx->path_depth > 0 && "bb_serialize_console: path stack underflow -- end_obj without a matching begin_obj (walker desync)");  // LCOV_EXCL_BR_LINE -- abort branch untestable, no death-test harness
    if (ctx->path_depth == 0) return;  // LCOV_EXCL_LINE -- unreachable when assert() is compiled in; NDEBUG-only safety net
    ctx->path_depth--;
}

// ---------------------------------------------------------------------------
// bb_serialize_emit_t callbacks
// ---------------------------------------------------------------------------

// bb_serialize_console_register_format() registers a ctx-less template
// vtable (ctx == NULL) into the format-dispatch registry -- a lookup caller
// MUST copy the template and bind a real ctx before walking it (see
// bb_serialize_format.h). vctx == NULL here means an unbound template was
// walked directly, a composition bug upstream (fail loud, never NULL-deref
// into ctx->len/ctx->cap below).
static bb_serialize_console_ctx_t *bb_console_cb_ctx(void *vctx)
{
    assert(vctx != NULL && "bb_serialize_console emit callback invoked with NULL ctx -- unbound template walked directly");  // LCOV_EXCL_BR_LINE -- abort branch untestable, no death-test harness
    return (bb_serialize_console_ctx_t *)vctx;
}

// BB_TYPE_OBJ nesting is qualification-aware (B1-1416, see
// bb_serialize_console_emit()'s doc comment): begin_obj pushes `key` onto
// the ctx's path stack (see bb_console_push_path()), end_obj pops it. No
// bracket characters are ever written -- the object boundary itself stays
// invisible on the line, only the accumulated path prefixes each nested
// scalar's own key.
static void bb_console_cb_begin_obj(void *vctx, const char *key)
{
    bb_console_push_path(bb_console_cb_ctx(vctx), key);
}

static void bb_console_cb_end_obj(void *vctx)
{
    bb_console_pop_path(bb_console_cb_ctx(vctx));
}

static void bb_console_cb_begin_arr(void *vctx, const char *key)
{
    (void)bb_console_cb_ctx(vctx);
    (void)key;
}

static void bb_console_cb_end_arr(void *vctx)
{
    (void)bb_console_cb_ctx(vctx);
}

// i64/u64 formatting is delegated to bb_num (portable decimal formatting,
// no snprintf-`ll`-conversion dependency -- see bb_console_appendf's file
// header comment for why: newlib-nano can't format "%lld"/"%llu"). Written
// directly into ctx->buf/ctx->len rather than through bb_console_appendf(),
// but honoring the same "avail = cap - len" bounded-append/truncation
// contract as every other emit path in this file.
static void bb_console_cb_emit_i64(void *vctx, const char *key, int64_t v)
{
    bb_serialize_console_ctx_t *ctx = bb_console_cb_ctx(vctx);
    bb_console_pre_value(ctx, key);
    size_t avail = ctx->cap - ctx->len;
    ctx->len += bb_num_i64_to_dec(ctx->buf + ctx->len, avail, v);
}

static void bb_console_cb_emit_u64(void *vctx, const char *key, uint64_t v)
{
    bb_serialize_console_ctx_t *ctx = bb_console_cb_ctx(vctx);
    bb_console_pre_value(ctx, key);
    size_t avail = ctx->cap - ctx->len;
    ctx->len += bb_num_u64_to_dec(ctx->buf + ctx->len, avail, v);
}

static void bb_console_cb_emit_f64(void *vctx, const char *key, double v)
{
    bb_serialize_console_ctx_t *ctx = bb_console_cb_ctx(vctx);
    bb_console_pre_value(ctx, key);
    bb_console_appendf(ctx, "%g", v);
}

static void bb_console_cb_emit_bool(void *vctx, const char *key, bool v)
{
    bb_serialize_console_ctx_t *ctx = bb_console_cb_ctx(vctx);
    bb_console_pre_value(ctx, key);
    bb_console_appendf(ctx, "%s", v ? "true" : "false");
}

static void bb_console_cb_emit_str(void *vctx, const char *key, const char *s, size_t len)
{
    bb_serialize_console_ctx_t *ctx = bb_console_cb_ctx(vctx);
    bb_console_pre_value(ctx, key);
    bb_console_appendf(ctx, "%.*s", (int)len, s);
}

static void bb_console_cb_emit_null(void *vctx, const char *key)
{
    bb_serialize_console_ctx_t *ctx = bb_console_cb_ctx(vctx);
    bb_console_pre_value(ctx, key);
    bb_console_appendf(ctx, "null");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void bb_serialize_console_ctx_init(bb_serialize_console_ctx_t *ctx, char *buf, size_t cap)
{
    ctx->buf = buf;
    ctx->cap = cap;
    ctx->len = 0;
    ctx->path_depth = 0;
    if (cap > 0) buf[0] = '\0';
}

bb_serialize_emit_t bb_serialize_console_emit(bb_serialize_console_ctx_t *ctx)
{
    bb_serialize_emit_t emit = {
        .format_id = BB_FORMAT_CONSOLE,
        .ctx = ctx,
        .begin_obj = bb_console_cb_begin_obj,
        .end_obj = bb_console_cb_end_obj,
        .begin_arr = bb_console_cb_begin_arr,
        .end_arr = bb_console_cb_end_arr,
        .emit_i64 = bb_console_cb_emit_i64,
        .emit_u64 = bb_console_cb_emit_u64,
        .emit_f64 = bb_console_cb_emit_f64,
        .emit_bool = bb_console_cb_emit_bool,
        .emit_str = bb_console_cb_emit_str,
        .emit_null = bb_console_cb_emit_null,
    };
    return emit;
}

bb_err_t bb_serialize_console_render(const bb_serialize_desc_t *desc, const void *snap,
                                      char *buf, size_t cap, size_t *out_len)
{
    if (!buf || cap == 0) {
        if (out_len) *out_len = 0;
        return BB_ERR_NO_SPACE;
    }

    bb_serialize_console_ctx_t ctx;
    bb_serialize_console_ctx_init(&ctx, buf, cap);

    bb_serialize_emit_t emit = bb_serialize_console_emit(&ctx);
    bb_serialize_walk_cfg_t walk_cfg = { .desc = desc, .snap = snap, .emit = &emit };
    bb_serialize_walk(&walk_cfg);

    // Every begin_obj push (bb_console_push_path()) must have been popped
    // by its matching end_obj (bb_console_pop_path()) by the time the walk
    // completes -- see bb_console_pop_path()'s comment for what a future
    // desync here would silently do to every subsequent render (B1-1416).
    assert(ctx.path_depth == 0 && "bb_serialize_console: path stack not fully unwound after walk (walker desync)");  // LCOV_EXCL_BR_LINE -- abort branch untestable, no death-test harness

    if (out_len) *out_len = ctx.len;
    return BB_OK;  // truncated-but-NUL-terminated is a success, not an error
}
