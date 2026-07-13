// Buffered GitHub release manifest parser.
//
// Drives bb_serialize_json_scan_bounded() (components/bb_serialize_json)
// against the SAME shared bb_release_manifest_sink_state_t sink used by the
// streaming parser (bb_release_manifest_github_stream.c) -- see
// bb_release_manifest_json_sink.h for the extraction rules and early-stop
// contract.
//
// This file used to be a bespoke find_key()/copy_string_value()/
// match_brace() hand-rolled scanner (see git history) -- retired in favor
// of the shared bb_serialize_json scanner alongside the streaming variant
// per the repo's consolidation rule.

#include "bb_release_manifest.h"
#include "bb_release_manifest_json_sink.h"

bb_err_t bb_release_manifest_parse_github(
    const char *body, size_t body_len,
    const char *board_name,
    char *out_tag, size_t tag_size,
    char *out_url, size_t url_size)
{
    if (!body || !board_name || !out_tag || !out_url) {
        return BB_ERR_INVALID_ARG;
    }
    if (tag_size == 0 || url_size == 0) {
        return BB_ERR_INVALID_ARG;
    }

    bb_release_manifest_sink_state_t st;
    bb_release_manifest_sink_init(&st, board_name, out_tag, tag_size, out_url, url_size);
    bb_serialize_json_ingest_t ingest = bb_release_manifest_sink_ingest(&st);

    bb_err_t rc = bb_serialize_json_scan_bounded(body, body_len, &ingest);
    return bb_release_manifest_sink_finalize(&st, rc);
}
