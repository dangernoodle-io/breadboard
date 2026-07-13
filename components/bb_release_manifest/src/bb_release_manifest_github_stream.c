// Streaming GitHub release manifest parser.
//
// Drives bb_serialize_json_scan_begin/_feed/_end() (components/bb_serialize_json)
// against the shared bb_release_manifest_sink_state_t sink
// (bb_release_manifest_json_sink.c/.h) -- callers never need to buffer the
// entire response body; see bb_release_manifest_json_sink.h for the
// extraction rules and early-stop contract.
//
// This file used to be a bespoke byte-driven phase-enum state machine (see
// git history) -- retired in favor of the shared bb_serialize_json scanner
// per the repo's consolidation rule (a second hand-rolled JSON scanner in
// bb_release_manifest_github.c triggered the extraction).

#include "bb_release_manifest.h"
#include "bb_release_manifest_json_sink.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Internal layout inside _state[BB_RELEASE_MANIFEST_STREAM_STATE_SIZE]
// ---------------------------------------------------------------------------

typedef struct {
    bb_release_manifest_sink_state_t sink;
    // Stored alongside (not just locally in _begin()) because
    // bb_serialize_json_scan_begin() retains a POINTER to this vtable for
    // the lifetime of the whole scan -- it must outlive every _feed()/_end()
    // call, not just the _begin() call that constructs it.
    bb_serialize_json_ingest_t       ingest;
    bb_serialize_json_scan_ctx_t     scan;
} stream_state_t;

// Compile-time size check -- see BB_RELEASE_MANIFEST_STREAM_STATE_SIZE's doc
// comment in bb_release_manifest.h for the full 64-bit-host vs 32-bit-device
// size breakdown (672 vs ~604 bytes) and the "grown deliberately" note: this
// struct (sink + ingest vtable + the embedded bb_serialize_json scanner's
// own 384-byte state) is larger than the pre-scanner state machine it
// replaces.
typedef char _stream_state_size_check[
    sizeof(stream_state_t) <= BB_RELEASE_MANIFEST_STREAM_STATE_SIZE ? 1 : -1
];

static inline stream_state_t *ctx_state(bb_release_manifest_stream_ctx_t *ctx)
{
    return (stream_state_t *)(void *)ctx->_state;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bb_err_t bb_release_manifest_parse_github_stream_begin(
    bb_release_manifest_stream_ctx_t *ctx,
    const char *board_fallback,
    char *tag_out, size_t tag_cap,
    char *url_out, size_t url_cap)
{
    if (!ctx || !board_fallback || !tag_out || !url_out) return BB_ERR_INVALID_ARG;
    if (tag_cap < 2 || url_cap < 2) return BB_ERR_INVALID_ARG;

    stream_state_t *s = ctx_state(ctx);
    bb_release_manifest_sink_init(&s->sink, board_fallback, tag_out, tag_cap, url_out, url_cap);
    s->ingest = bb_release_manifest_sink_ingest(&s->sink);

    return bb_serialize_json_scan_begin(&s->scan, &s->ingest);
}

bb_err_t bb_release_manifest_parse_github_stream_feed(
    bb_release_manifest_stream_ctx_t *ctx,
    const char *chunk, size_t chunk_len)
{
    if (!ctx || !chunk) return BB_ERR_INVALID_ARG;  // LCOV_EXCL_BR_LINE — defensive

    stream_state_t *s = ctx_state(ctx);

    // bb_serialize_json_scan_feed() is itself sticky once the scan has
    // stopped (early-stop sentinel OR a real grammar error): it short-
    // circuits to the stored code without touching `chunk` at all, so no
    // separate "done" flag is needed here -- see
    // bb_serialize_json_scan_feed()'s doc comment in bb_serialize_json.h.
    bb_err_t rc = bb_serialize_json_scan_feed(&s->scan, chunk, chunk_len);

    // Only a real buffer-overflow (BB_ERR_NO_SPACE -- either the tag/url
    // output buffer or the scanner's own key/number scratch) legitimately
    // aborts the transport, matching the pre-scanner contract exactly:
    // the old hand-rolled state machine only ever produced BB_ERR_NO_SPACE
    // as a real _feed() error. It never validated JSON grammar, so a
    // malformed body was simply consumed leniently and the eventual
    // outcome surfaced at _end() time. The new scanner DOES detect
    // malformed grammar as it goes, but a mid-stream grammar error is
    // deliberately masked back to BB_OK here (not propagated) to preserve
    // that exact shape for the transport -- bb_release_manifest_sink_finalize()
    // (driven from _end(), below) still asks the scanner for its final
    // sticky code and folds a grammar error into BB_ERR_NOT_FOUND (via
    // tag_found), so a genuinely malformed body is still correctly
    // reported, just at _end() rather than at the _feed() call that first
    // noticed it -- identical to the pre-scanner behavior. The early-stop
    // sentinel (BB_RELEASE_MANIFEST_SINK_DONE) is masked the same way: it
    // means "found everything, no need to keep feeding", not an error.
    if (rc == BB_ERR_NO_SPACE) return BB_ERR_NO_SPACE;
    return BB_OK;
}

bb_err_t bb_release_manifest_parse_github_stream_end(
    bb_release_manifest_stream_ctx_t *ctx)
{
    if (!ctx) return BB_ERR_INVALID_ARG;  // LCOV_EXCL_BR_LINE — defensive

    stream_state_t *s = ctx_state(ctx);

    // bb_serialize_json_scan_end() is sticky the same way _feed() is: if the
    // scan already stopped (sentinel or error), it returns that code
    // immediately without re-validating the (possibly still-open)
    // container stack -- exactly what we want, since an early-stopped scan
    // never reaches PH_AFTER_ROOT and would otherwise spuriously report
    // BB_ERR_INVALID_STATE ("open container at end").
    bb_err_t rc = bb_serialize_json_scan_end(&s->scan);
    return bb_release_manifest_sink_finalize(&s->sink, rc);
}
