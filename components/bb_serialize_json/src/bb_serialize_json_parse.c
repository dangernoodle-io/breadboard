// Composed parse adapter -- bb_serialize_json_parse_bytes(), the JSON
// backend's registered bb_serialize_parse_fn (see bb_serialize_format.h).
// See bb_serialize_json_parse_bytes()'s doc comment in bb_serialize_json.h
// for the full scratch-layout/lifetime contract.
#include "bb_serialize_json.h"

#include "bb_mem_arena.h"

bb_err_t bb_serialize_json_parse_bytes(const char *buf, size_t len,
                                        void *scratch, size_t scratch_cap,
                                        bb_serialize_populate_t *out_src)
{
    if (!buf || !scratch || !out_src) return BB_ERR_INVALID_ARG;

    // bb_mem_arena carves its own small header off the front of `scratch`,
    // then bump-allocates the rest -- every allocation below lands INSIDE
    // `scratch` (max_align_t-aligned), never on this fn's own stack, which
    // is what lets the recorder/cursor bound into `*out_src` legitimately
    // outlive this call.
    bb_mem_arena_t arena;
    if (bb_mem_arena_init(&arena, scratch, scratch_cap) != BB_OK) return BB_ERR_NO_SPACE;

    bb_serialize_json_tok_recorder_t *rec = bb_mem_arena_alloc(arena, sizeof(*rec));
    bb_serialize_json_populate_ctx_t *pctx = bb_mem_arena_alloc(arena, sizeof(*pctx));
    bb_serialize_json_tok_t          *pool =
        bb_mem_arena_alloc(arena, BB_SERIALIZE_JSON_TOK_POOL_DEFAULT_CAP * sizeof(*pool));
    if (!rec || !pctx || !pool) return BB_ERR_NO_SPACE;

    // Whatever's left becomes the recorder's escape-decode arena -- optional
    // (NULL/0 is a valid, if escape-intolerant, configuration; see
    // bb_serialize_json_tok_recorder_init()'s doc). bb_mem_arena_alloc_rest()
    // (NOT a plain bb_mem_arena_alloc(a, bb_mem_arena_free_bytes(a))) --
    // that naive exact-free-bytes form spuriously fails the WHOLE parse
    // whenever the remainder isn't already alignment-clean, since
    // bb_mem_arena_alloc() rounds its request UP before checking it against
    // the true remaining bytes. alloc_rest() rounds DOWN instead, so it can
    // never fail here for that reason -- a NULL/0 result is a legitimate
    // "nothing (alignable) left" outcome, not a caller error.
    size_t json_arena_cap = 0;
    char  *json_arena      = bb_mem_arena_alloc_rest(arena, &json_arena_cap);

    bb_err_t rc = bb_serialize_json_tok_recorder_init(rec, buf, len, pool,
                                                        BB_SERIALIZE_JSON_TOK_POOL_DEFAULT_CAP,
                                                        json_arena, json_arena_cap);
    if (rc != BB_OK) return rc;  // LCOV_EXCL_LINE -- unreachable: all recorder_init preconditions hold here

    bb_serialize_json_ingest_t sink = bb_serialize_json_tok_recorder_ingest(rec);
    rc = bb_serialize_json_scan_bounded(buf, len, &sink);
    if (rc != BB_OK) return rc;

    *out_src = bb_serialize_json_populate_from_tok(pctx, rec);
    return BB_OK;
}
