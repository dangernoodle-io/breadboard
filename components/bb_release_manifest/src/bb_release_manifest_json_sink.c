// Shared bb_serialize_json_ingest_t sink for both GitHub release-manifest
// parsers -- see bb_release_manifest_json_sink.h for the full contract.

#include "bb_release_manifest_json_sink.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Append helpers
// ---------------------------------------------------------------------------

// Appends `chunk`/`chunk_len` to `dst` (capacity `cap`, NUL-terminated,
// `*len` tracks bytes written so far excluding the NUL). Returns
// BB_ERR_NO_SPACE without writing anything if it would overflow. A
// zero-length chunk is always safe (the task's documented "zero-length
// is_final chunk for a non-empty string" case) -- handled explicitly before
// the capacity check so it can never false-positive when the buffer is
// already exactly full.
static bb_err_t sink_append(char *dst, size_t cap, size_t *len,
                             const char *chunk, size_t chunk_len)
{
    if (chunk_len == 0) return BB_OK;
    if (*len + chunk_len + 1 > cap) return BB_ERR_NO_SPACE;
    memcpy(dst + *len, chunk, chunk_len);
    *len += chunk_len;
    dst[*len] = '\0';
    return BB_OK;
}

// Same shape as sink_append(), but silently truncates instead of erroring --
// used only for the asset "name" comparison scratch, where an oversized
// value should simply fail to match (never abort the scan). Mirrors the old
// hand-rolled parsers' copy_tmp_char()/silent-truncate behavior.
static void sink_append_truncate(char *dst, size_t cap, size_t *len,
                                  const char *chunk, size_t chunk_len)
{
    if (*len + 1 >= cap) return;  // already full
    size_t avail = cap - *len - 1;
    size_t n = chunk_len < avail ? chunk_len : avail;
    if (n > 0) {
        memcpy(dst + *len, chunk, n);
        *len += n;
        dst[*len] = '\0';
    }
}

// ---------------------------------------------------------------------------
// bb_serialize_json_ingest_t callbacks
// ---------------------------------------------------------------------------

// Returns true if `key`/`key_len` is the top-level "assets" key (depth==1,
// object member of the root). Shared by sink_begin_obj() and the scalar
// value callbacks below to detect a structurally invalid "assets" value
// (an object or scalar instead of an array) -- see assets_invalid_shape's
// doc comment in bb_release_manifest_json_sink.h.
static bool is_toplevel_assets_key(int depth, const char *key, size_t key_len)
{
    return depth == 1 && key != NULL && key_len == 6 &&
           memcmp(key, "assets", 6) == 0;
}

static bb_err_t sink_begin_obj(void *ctx, const char *key, size_t key_len)
{
    bb_release_manifest_sink_state_t *s = ctx;

    // "assets" key present but its value is an OBJECT, not an array --
    // structurally invalid; restores the old buffered parser's hard-fail
    // (see bb_release_manifest_sink_finalize()).
    if (!s->in_assets && is_toplevel_assets_key(s->depth, key, key_len)) {
        s->assets_invalid_shape = true;
    }

    // Entering an asset element: array elements carry no key, so this is
    // detected purely by depth -- s->depth (pre-increment, below) equals
    // the element depth recorded when the "assets" array was opened.
    if (s->in_assets && !s->in_asset_obj && s->depth == s->assets_elem_depth) {
        s->in_asset_obj       = true;
        s->asset_obj_depth    = s->depth;
        s->asset_name_matched = false;
        s->name_tmp_len       = 0;
    }
    s->depth++;
    return BB_OK;
}

static bb_err_t sink_end_obj(void *ctx)
{
    bb_release_manifest_sink_state_t *s = ctx;
    s->depth--;
    if (s->in_asset_obj && s->depth == s->asset_obj_depth) {
        s->in_asset_obj       = false;
        s->asset_name_matched = false;
    }
    return BB_OK;
}

static bb_err_t sink_begin_arr(void *ctx, const char *key, size_t key_len)
{
    bb_release_manifest_sink_state_t *s = ctx;

    // Top-level "assets" array: depth==1 means directly inside the root
    // object (root's own begin_obj already incremented depth to 1). The
    // key!=NULL sub-check is NOT purely defensive: bb_serialize_json is a
    // general scanner with no requirement that the ROOT value be an
    // object -- a top-level JSON ARRAY dispatches begin_arr() for its own
    // elements at depth==1 with key==NULL (an array element carries no
    // key), which is exactly the combination this sub-check excludes. It
    // is real and reachable (see
    // test_bb_release_manifest_parse_github_root_array_document), just
    // harmless: the condition simply evaluates false and that element is
    // ignored like any other non-"assets" value.
    if (!s->in_assets && is_toplevel_assets_key(s->depth, key, key_len)) {
        s->in_assets         = true;
        s->assets_arr_depth  = s->depth;
        s->assets_elem_depth = s->depth + 1;
    }
    s->depth++;
    return BB_OK;
}

static bb_err_t sink_end_arr(void *ctx)
{
    bb_release_manifest_sink_state_t *s = ctx;
    s->depth--;
    if (s->in_assets && s->depth == s->assets_arr_depth) {
        s->in_assets = false;
    }
    return BB_OK;
}

// Marks assets_invalid_shape when `key`/`key_len` is the top-level "assets"
// key seen on a SCALAR value (number/bool/null) -- the array/object shapes
// are marked from sink_begin_arr()/sink_begin_obj() instead. Shared by the
// three scalar callbacks below.
static void mark_if_assets_scalar(bb_release_manifest_sink_state_t *s,
                                   const char *key, size_t key_len)
{
    if (!s->in_assets && is_toplevel_assets_key(s->depth, key, key_len)) {
        s->assets_invalid_shape = true;
    }
}

static bb_err_t sink_value_num(void *ctx, const char *key, size_t key_len,
                                const char *num, size_t num_len)
{
    (void)num; (void)num_len;
    mark_if_assets_scalar(ctx, key, key_len);
    return BB_OK;
}

static bb_err_t sink_value_bool(void *ctx, const char *key, size_t key_len, bool v)
{
    (void)v;
    mark_if_assets_scalar(ctx, key, key_len);
    return BB_OK;
}

static bb_err_t sink_value_null(void *ctx, const char *key, size_t key_len)
{
    mark_if_assets_scalar(ctx, key, key_len);
    return BB_OK;
}

static bb_err_t sink_value_str_chunk(void *ctx, const char *key, size_t key_len,
                                      const char *chunk, size_t chunk_len, bool is_final,
                                      bb_serialize_json_span_t span_provenance)
{
    // Every span (CALLER_STABLE/CALLER_FEED_SCOPED/SCANNER_SCRATCH alike)
    // is copied out via sink_append()/sink_append_truncate() below before
    // this callback returns -- satisfies the "copy before returning"
    // requirement for the streaming (FEED_SCOPED) and scratch cases, and is
    // simply the cheapest correct thing to do in the bounded (STABLE) case
    // too, since the same sink drives both entry points.
    (void)span_provenance;

    bb_release_manifest_sink_state_t *s = ctx;

    // is_tag's depth==1/key!=NULL sub-checks are NOT structurally
    // unreachable -- same reasoning as sink_begin_arr()'s comment: a
    // top-level JSON ARRAY dispatches this callback for its own string
    // elements at depth==1 with key==NULL, which is real (see
    // test_bb_release_manifest_parse_github_root_array_document) and
    // simply makes is_tag false, same as any other non-"tag_name" value.
    //
    // is_name/is_url's depth==asset_obj_depth+1 sub-checks ARE genuinely
    // unreachable with key!=NULL false: both are gated by s->in_asset_obj,
    // which is only ever true while `depth` places this call directly
    // inside an asset object member (the scanner's key/NULL convention
    // guarantees a non-NULL key there), so "in_asset_obj && key==NULL" at
    // that specific depth can never actually occur -- residual branches
    // on those lines reflect that combination specifically (every other
    // sub-condition on those lines IS exercised, both true and false, by
    // the many matching/non-matching/duplicate-key/nested-key tests in
    // this component's test suite -- see test_bb_release_manifest.c).
    bool is_tag = (!s->tag_found && s->depth == 1 &&
                   key != NULL && key_len == 8 && memcmp(key, "tag_name", 8) == 0);
    bool is_name = (s->in_asset_obj && !s->asset_name_matched &&
                     s->depth == s->asset_obj_depth + 1 &&  // LCOV_EXCL_BR_LINE — see comment above
                     key != NULL && key_len == 4 && memcmp(key, "name", 4) == 0);
    bool is_url = (s->in_asset_obj && s->asset_name_matched && !s->url_found &&
                    s->depth == s->asset_obj_depth + 1 &&  // LCOV_EXCL_BR_LINE — see comment above
                    key != NULL && key_len == 20 &&
                    memcmp(key, "browser_download_url", 20) == 0);

    mark_if_assets_scalar(s, key, key_len);

    if (is_tag) {
        bb_err_t rc = sink_append(s->tag_out, s->tag_cap, &s->tag_len, chunk, chunk_len);
        if (rc != BB_OK) return rc;
        if (is_final) {
            s->tag_found = true;
            if (s->url_found) return BB_RELEASE_MANIFEST_SINK_DONE;
        }
    } else if (is_name) {
        sink_append_truncate(s->name_tmp, sizeof(s->name_tmp), &s->name_tmp_len, chunk, chunk_len);
        if (is_final) {
            s->asset_name_matched =
                (s->name_tmp_len == s->board_name_len) &&
                memcmp(s->name_tmp, s->board_name, s->board_name_len) == 0;
        }
    } else if (is_url) {
        bb_err_t rc = sink_append(s->url_out, s->url_cap, &s->url_len, chunk, chunk_len);
        if (rc != BB_OK) return rc;
        if (is_final) {
            s->url_found = true;
            if (s->tag_found) return BB_RELEASE_MANIFEST_SINK_DONE;
        }
    }
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Public (component-private) API
// ---------------------------------------------------------------------------

void bb_release_manifest_sink_init(bb_release_manifest_sink_state_t *st,
                                    const char *board_fallback,
                                    char *tag_out, size_t tag_cap,
                                    char *url_out, size_t url_cap)
{
    memset(st, 0, sizeof(*st));
    st->tag_out = tag_out;
    st->tag_cap = tag_cap;
    st->url_out = url_out;
    st->url_cap = url_cap;
    tag_out[0]  = '\0';
    url_out[0]  = '\0';

    // Build "<board_fallback>.bin" for asset name matching, truncating if
    // it doesn't fit -- an oversized/truncated name simply never matches,
    // same fallback as the pre-scanner implementations.
    size_t bl = strlen(board_fallback);
    size_t full_len = bl + 4;  // + ".bin"
    if (full_len >= sizeof(st->board_name)) {
        full_len = sizeof(st->board_name) - 1;
        bl = full_len - 4;
    }
    memcpy(st->board_name, board_fallback, bl);
    memcpy(st->board_name + bl, ".bin", 4);
    st->board_name[full_len] = '\0';
    st->board_name_len = full_len;
}

bb_serialize_json_ingest_t bb_release_manifest_sink_ingest(bb_release_manifest_sink_state_t *st)
{
    bb_serialize_json_ingest_t sink = {
        .ctx = st,
        .begin_obj = sink_begin_obj,
        .end_obj = sink_end_obj,
        .begin_arr = sink_begin_arr,
        .end_arr = sink_end_arr,
        .value_num = sink_value_num,
        .value_bool = sink_value_bool,
        .value_null = sink_value_null,
        .value_str_chunk = sink_value_str_chunk,
    };
    return sink;
}

bb_err_t bb_release_manifest_sink_finalize(const bb_release_manifest_sink_state_t *st,
                                            bb_err_t scan_result)
{
    if (scan_result == BB_RELEASE_MANIFEST_SINK_DONE) return BB_OK;
    if (scan_result == BB_ERR_NO_SPACE) return BB_ERR_NO_SPACE;

    // Never let a partial value survive into a caller-visible output
    // buffer. A *_found flag is only set from the is_final==true arm of
    // sink_value_str_chunk(); if it is still false here, tag_out/url_out
    // is either untouched ("" from bb_release_manifest_sink_init(), and
    // this is a no-op) OR holds a PARTIAL value because the scan was cut
    // off mid-string (e.g. a truncated HTTP connection mid
    // "browser_download_url", which bb_serialize_json_scan_end() reports
    // as a genuine BB_ERR_INVALID_STATE grammar error, distinct from the
    // sentinel/NO_SPACE cases already handled above). Clearing
    // unconditionally makes both cases indistinguishable to the caller --
    // exactly what we want, since neither is a "found" value.
    if (!st->tag_found) st->tag_out[0] = '\0';
    if (!st->url_found) st->url_out[0] = '\0';

    // A genuine scan/grammar error (anything other than BB_OK left over
    // once the SINK_DONE sentinel and BB_ERR_NO_SPACE have already been
    // handled above -- e.g. bb_serialize_json_scan_end()'s
    // BB_ERR_INVALID_STATE for a truncated/unterminated value, or
    // BB_ERR_VALIDATION for outright malformed grammar) means the fetch
    // itself was dropped or corrupt. That is a broken/unexpected upstream
    // response, not "no asset for this board" -- hard-fail rather than
    // folding it into the benign no-asset terminal below, even if
    // tag_name was already extracted before the error occurred. Silently
    // returning BB_OK here would make a dropped/corrupt GitHub response
    // indistinguishable from a well-formed manifest with no matching
    // asset (bb_ota_check_common.c's dl_url[0]=='\0' no-asset guard would
    // fire either way) and swallow a real fetch failure as "no update
    // available", never alerting on it.
    if (scan_result != BB_OK) return BB_ERR_NOT_FOUND;

    // A structurally invalid "assets" value (an object or scalar instead
    // of an array, e.g. `"assets":"oops"`) is a broken/unexpected upstream
    // response shape, not "no asset for this board" -- hard-fail rather
    // than folding it into the benign no-asset terminal below, even if
    // tag_name was already extracted.
    if (st->assets_invalid_shape) return BB_ERR_NOT_FOUND;

    return st->tag_found ? BB_OK : BB_ERR_NOT_FOUND;
}
