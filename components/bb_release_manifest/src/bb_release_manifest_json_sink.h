#pragma once

// Private (non-public) sink shared by both GitHub release-manifest parsers
// -- bb_release_manifest_github_stream.c (streaming) and
// bb_release_manifest_github.c (bounded) both drive the SAME
// bb_serialize_json_ingest_t sink implementation here, one over
// bb_serialize_json_scan_begin/_feed/_end, the other over
// bb_serialize_json_scan_bounded(). Not installed under include/ -- this is
// an internal implementation detail of this component, never a public API.
//
// Extraction rules (identical to the pre-scanner implementations):
//   1. Top-level "tag_name" value -> tag_out
//   2. Under assets[], the object whose "name" == "<board_fallback>.bin" ->
//      that object's "browser_download_url" -> url_out
//
// Early-stop: once both tag_name and the matching URL are found, a
// value_str_chunk callback returns BB_RELEASE_MANIFEST_SINK_DONE -- a
// private sentinel bb_err_t, never a real error -- to abort the scan
// immediately rather than chewing the rest of the document. Both driving
// .c files MUST route the scanner's return code through
// bb_release_manifest_sink_finalize() before handing anything back to a
// caller: that is the ONLY place the sentinel is translated back to BB_OK,
// so it never leaks out of this component's public API.

#include "bb_release_manifest.h"
#include "bb_serialize_json.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Capacity for the board-fallback match name ("<board>.bin") and the
// scratch buffer used to accumulate an asset's "name" value for comparison.
// Sized well above any realistic firmware asset filename (the longest in
// this repo's own release payloads is ~30 chars); a name that overflows
// this is silently truncated (never matches, never errors -- same
// "no match" fallback as the pre-scanner implementation).
#define BB_RELEASE_MANIFEST_SINK_NAME_MAX 64

// Private sentinel bb_err_t used ONLY between this sink's value_str_chunk
// callback and bb_release_manifest_sink_finalize() (below) in the SAME
// scan -- an early-stop signal, not a real error. Reuses BB_ERR_CONFLICT
// (unused by both the scanner and this component's own error paths) rather
// than inventing a new bb_err_t constant; never returned across this
// component's public API -- bb_release_manifest_sink_finalize() maps it
// (and every scanner completion outcome) to a real public bb_err_t.
#define BB_RELEASE_MANIFEST_SINK_DONE BB_ERR_CONFLICT

typedef struct {
    // Outputs (borrowed from caller; must outlive the scan)
    char   *tag_out;
    size_t  tag_cap;
    size_t  tag_len;
    char   *url_out;
    size_t  url_cap;
    size_t  url_len;

    // Board name to match, stored as "<board_fallback>.bin"
    char    board_name[BB_RELEASE_MANIFEST_SINK_NAME_MAX];
    size_t  board_name_len;

    // Scratch for the current asset object's "name" value
    char    name_tmp[BB_RELEASE_MANIFEST_SINK_NAME_MAX];
    size_t  name_tmp_len;

    // Depth bookkeeping -- see bb_release_manifest_json_sink.c for the
    // exact depth arithmetic.
    int     depth;
    int     assets_arr_depth;
    int     assets_elem_depth;
    int     asset_obj_depth;

    bool    tag_found;
    bool    url_found;
    bool    in_assets;
    bool    in_asset_obj;
    bool    asset_name_matched;

    // Set when a top-level "assets" key is observed carrying a value that
    // is NOT an array (a scalar or an object) -- e.g. `"assets":"oops"`.
    // That shape signals a broken/unexpected upstream API response, not
    // "no asset published for this board", and bb_release_manifest_sink_finalize()
    // hard-fails on it (BB_ERR_NOT_FOUND) rather than folding it into the
    // benign "no matching asset" terminal. See bb_release_manifest_json_sink.c.
    bool    assets_invalid_shape;
} bb_release_manifest_sink_state_t;

// Initializes `st` to match `board_fallback` (as "<board_fallback>.bin")
// and write into `tag_out`/`url_out`. Mirrors the argument validation
// already performed by both public _begin()/parse() entry points -- callers
// validate before calling this.
void bb_release_manifest_sink_init(bb_release_manifest_sink_state_t *st,
                                    const char *board_fallback,
                                    char *tag_out, size_t tag_cap,
                                    char *url_out, size_t url_cap);

// Returns a bb_serialize_json_ingest_t vtable bound to `st`. The RETURNED
// STRUCT ITSELF must be stored somewhere that outlives the scan (the
// scanner retains a POINTER to it, not a copy) -- callers must not pass a
// stack temporary of this return value across a _feed()/_end() boundary;
// see the two driving .c files for the pattern (store it as a field
// alongside `st`, not a local in the calling function).
bb_serialize_json_ingest_t bb_release_manifest_sink_ingest(bb_release_manifest_sink_state_t *st);

// Translates a bb_serialize_json_scan_* completion code (from
// bb_serialize_json_scan_end() or bb_serialize_json_scan_bounded(), or a
// raw bb_serialize_json_scan_feed() error when the caller stopped feeding
// early) into the public bb_release_manifest contract:
//   BB_RELEASE_MANIFEST_SINK_DONE -> BB_OK (early-stop: both fields found)
//   BB_ERR_NO_SPACE               -> BB_ERR_NO_SPACE (tag/url buffer or
//                                     scanner scratch overflow)
//   assets_invalid_shape          -> BB_ERR_NOT_FOUND, unconditionally (a
//                                     structurally broken "assets" value is
//                                     a hard failure, never folded into the
//                                     benign no-asset terminal below)
//   anything else (BB_OK, or a scanner grammar error e.g. BB_ERR_VALIDATION/
//   BB_ERR_INVALID_STATE) -> BB_OK if tag_name was extracted, else
//                             BB_ERR_NOT_FOUND -- folds "malformed JSON"
//                             into the same "not found" bucket the
//                             pre-scanner lenient parser used, rather than
//                             leaking scanner-internal grammar codes into
//                             this component's public API.
//
// Before deciding the result, this ALWAYS clears (to "") whichever of
// tag_out/url_out does not have its *_found flag set. That flag is only
// ever set from the is_final==true arm of sink_value_str_chunk(), so a
// field can be un-found here for two indistinguishable-by-design reasons:
// it never appeared in the document at all (tag_out/url_out already holds
// "" from bb_release_manifest_sink_init(), so the clear is a no-op), or its
// value was PARTIALLY written into the caller's buffer and then the scan
// was cut off before is_final (e.g. a truncated HTTP connection mid
// "browser_download_url") -- the clear is what prevents that partial value
// from ever being visible to a caller. Every public entry point
// (bb_release_manifest_parse_github_stream_end() and
// bb_release_manifest_parse_github()) routes through this function before
// returning, so no caller can observe tag_out/url_out before this clear
// has run.
bb_err_t bb_release_manifest_sink_finalize(const bb_release_manifest_sink_state_t *st,
                                            bb_err_t scan_result);

#ifdef __cplusplus
}
#endif
