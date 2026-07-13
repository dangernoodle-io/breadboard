#pragma once

/**
 * @brief Hand-rolled, no-heap, bounded-buffer JSON bb_serialize_emit_t
 * backend -- the default wire-format implementation for bb_serialize.
 *
 * bb_serialize_json_render() drives bb_serialize_walk() against a caller
 * descriptor+snapshot pair and writes JSON directly into a caller-owned
 * fixed buffer. All-or-nothing: on any overflow the whole call fails
 * (BB_ERR_NO_SPACE) rather than returning truncated/partial JSON. No heap,
 * no snprintf, no locale-dependent formatting -- suitable for constrained
 * (no-PSRAM) targets and hot paths.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bb_core.h"
#include "bb_serialize.h"

#ifdef __cplusplus
extern "C" {
#endif

// One container-nesting level of writer state: whether the level is a JSON
// array (unkeyed elements) or object (keyed members), and whether a prior
// sibling has already been written (drives the leading-comma decision).
typedef struct {
    bool is_array;
    bool have_child;
} bb_serialize_json_level_t;

// Writer state -- caller-owned, no heap. `buf`/`cap` are supplied by the
// caller via bb_serialize_json_ctx_init(); `cap` is the FULL buffer
// capacity as given -- bb_serialize_json_render() reserves the final byte
// for the NUL terminator internally before initializing this context.
typedef struct {
    char    *buf;
    size_t   cap;    // usable capacity (excludes any reserved NUL byte)
    size_t   len;    // bytes written so far, excludes NUL
    bb_err_t err;     // sticky; BB_OK until the first overflow
    uint8_t  depth;   // index of the current (top) level in `stack`
    // A walker recursion level can cost UP TO TWO JSON containers, not one:
    // a BB_TYPE_ARR field with elem_type == BB_TYPE_OBJ opens an array
    // frame (begin_arr) AND, per element, an object frame (begin_obj)
    // before the walker's depth guard advances -- so a chain of nested
    // arr-of-obj fields costs 2 stack frames per BB_SERIALIZE_MAX_DEPTH
    // level. Sized 2*BB_SERIALIZE_MAX_DEPTH+2 (+2 for the root frame plus
    // slack) to cover that worst case; bb_json_push_level() also enforces
    // this bound at runtime (BB_ERR_NO_SPACE, no write) so a future change
    // to either constant can't silently reintroduce an OOB write.
    bb_serialize_json_level_t stack[2 * BB_SERIALIZE_MAX_DEPTH + 2];
} bb_serialize_json_ctx_t;

// Initializes `ctx` to an empty writer over `buf`/`cap`. Does not write
// anything (no opening brace) -- use bb_serialize_json_render() for the
// one-shot descriptor-driven entry point, or drive bb_serialize_json_emit()
// directly for manual control.
void bb_serialize_json_ctx_init(bb_serialize_json_ctx_t *ctx, char *buf, size_t cap);

// Returns a bb_serialize_emit_t vtable bound to `ctx` (format_id ==
// BB_FORMAT_JSON). Pass the result to bb_serialize_walk().
bb_serialize_emit_t bb_serialize_json_emit(bb_serialize_json_ctx_t *ctx);

// One-shot entry point: walks `desc`/`snap` and writes a complete JSON
// object (wrapped in `{`...`}`) into `buf` (capacity `cap`, including room
// for the NUL terminator). All-or-nothing: on success, `*out_len` is the
// written length (excluding NUL) and `buf` is NUL-terminated; on
// BB_ERR_NO_SPACE, `*out_len` is 0 and `buf[0]` is '\0' -- never partial
// JSON.
bb_err_t bb_serialize_json_render(const bb_serialize_desc_t *desc, const void *snap,
                                   char *buf, size_t cap, size_t *out_len);

// Same as bb_serialize_json_render(), plus BB_TYPE_REF resolution: drives
// bb_serialize_walk_ref() (rather than bb_serialize_walk()) with `resolve`/
// `resolve_ctx`, so a REF field's sibling section renders inline at its
// wire key. All-or-nothing semantics and the NUL-terminator/overflow
// contract are identical to bb_serialize_json_render().
bb_err_t bb_serialize_json_render_ref(const bb_serialize_desc_t *desc, const void *snap,
                                       char *buf, size_t cap, size_t *out_len,
                                       bb_serialize_ref_resolve_fn resolve, void *resolve_ctx);

// Computes a worst-case (upper-bound) byte count for rendering `desc` as
// JSON via bb_serialize_json_render() -- a sizing helper for callers that
// need to allocate/reserve a buffer statically. Returns SIZE_MAX if any
// subfield's width is unbounded (a BB_TYPE_STR_N or BB_TYPE_ARR field whose
// max_len/max_items hint is left at 0), OR if any subfield is a BB_TYPE_REF
// (a REF's rendered size depends on the resolver's sibling descriptor, which
// isn't known statically here -- always contributes an unbounded/unknown
// estimate).
size_t bb_serialize_json_bound(const bb_serialize_desc_t *desc);

// ---------------------------------------------------------------------------
// Ingest vtable + streaming/bounded scanner -- the read-side counterpart to
// bb_serialize_emit_t above. Deliberately NOT promoted into bb_serialize
// core: no second format consumer exists yet, so bb_serialize_json owns this
// seam alone (promote later if one appears).
//
// Callbacks return bb_err_t (unlike bb_serialize_emit_t's void callbacks) so
// a sink can early-stop/reject a parse; a non-BB_OK return is propagated
// back out of the scanner verbatim and scanning stops immediately -- same
// idiom as bb_http_client_chunk_cb.
//
// `key`/`key_len` is NULL/0 for array elements and the root; non-NULL for an
// object member (mirrors bb_serialize_emit_t's own key convention).
//
// String values are streamed as spans (`value_str_chunk`), never
// reassembled -- a string's length is unbounded (e.g. a download URL), so
// buffering it would impose an artificial cap or need unbounded scratch.
// Every span handed to a sink is already valid decoded UTF-8: the scanner
// NEVER exposes a naked backslash or a half-decoded `\uXXXX` escape,
// regardless of where a chunk boundary falls. `is_final` marks the last
// call for a given string (true on the call that reaches the closing
// quote); an empty string still gets exactly one call (zero-length,
// is_final=true).
//
// POINTER PROVENANCE/LIFETIME (`bb_serialize_json_span_t`) -- read this
// before storing `chunk`/`chunk_len` beyond the callback's own stack frame.
// This is a PROVENANCE signal (where the pointer came from), not a LIFETIME
// bool -- provenance and lifetime coincide in bounded mode but NOT in
// streaming mode, so a 2-valued "is this durable" flag cannot represent it;
// a sink that infers "false == durable" is wrong in streaming mode.
//   - BB_SERIALIZE_JSON_SPAN_CALLER_STABLE: `chunk` is a direct slice of
//     the caller's own long-lived `buf` passed to
//     bb_serialize_json_scan_bounded(). This ONLY ever occurs in bounded
//     mode. The span remains valid for as long as the caller keeps `buf`
//     alive -- safe to record as a (pointer, length) or (offset, length)
//     pair beyond the callback.
//   - BB_SERIALIZE_JSON_SPAN_CALLER_FEED_SCOPED: `chunk` is a direct slice
//     of the current `chunk` argument to bb_serialize_json_scan_feed().
//     This ONLY ever occurs in streaming mode. The span is valid ONLY for
//     the duration of THIS _feed() call -- it dies the moment _feed()
//     returns, same as the `chunk` argument itself. The sink MUST copy the
//     bytes out (or otherwise fully consume them) before returning, or
//     wire the copy through on `is_final`/next call. This is unchanged from
//     the scanner's existing streaming contract, not a new constraint.
//   - BB_SERIALIZE_JSON_SPAN_SCANNER_SCRATCH: `chunk` points into a small
//     on-stack scratch buffer owned by the scanner's current call frame (a
//     decoded escape sequence, e.g. `\n` or a `\uXXXX`/surrogate-pair
//     decode). This pointer is valid ONLY for the duration of the
//     callback -- the sink MUST copy the bytes out (or otherwise fully
//     consume them) before returning. Storing this pointer for later use
//     is a dangling-pointer bug, in EVERY mode, bounded or streaming.
// A single escape positioned immediately before the closing quote (e.g. a
// string that is exactly `"\n"`) collapses to ONE value_str_chunk call with
// is_final=true -- the same call-count/is_final shape as the escape-free
// direct-span case -- but the provenance value distinguishes them:
// CALLER_STABLE/CALLER_FEED_SCOPED for the direct-span case,
// SCANNER_SCRATCH for the escape-collapse case. Do not infer span safety
// from call count or is_final alone; the provenance value is the only
// signal that carries it.
//
// Keys, numbers, and true/false/null are the opposite: all three are
// provably bounded (keys are schema-known, JSON number grammar is bounded,
// literals are <=5 bytes), so they ARE always fully reassembled and
// delivered whole in a single callback (`value_num`'s `num`/`num_len` is the
// raw, unparsed number text).
typedef enum {
    BB_SERIALIZE_JSON_SPAN_CALLER_STABLE,      // bounded mode: slice of the caller's buf; durable
    BB_SERIALIZE_JSON_SPAN_CALLER_FEED_SCOPED, // streaming mode: slice of the current _feed() chunk; callback-only
    BB_SERIALIZE_JSON_SPAN_SCANNER_SCRATCH,    // decoded-escape scratch, both modes; callback-only
} bb_serialize_json_span_t;

typedef struct {
    void *ctx;
    bb_err_t (*begin_obj)(void *ctx, const char *key, size_t key_len);
    bb_err_t (*end_obj)  (void *ctx);
    bb_err_t (*begin_arr)(void *ctx, const char *key, size_t key_len);
    bb_err_t (*end_arr)  (void *ctx);
    bb_err_t (*value_num) (void *ctx, const char *key, size_t key_len, const char *num, size_t num_len);
    bb_err_t (*value_bool)(void *ctx, const char *key, size_t key_len, bool v);
    bb_err_t (*value_null)(void *ctx, const char *key, size_t key_len);
    bb_err_t (*value_str_chunk)(void *ctx, const char *key, size_t key_len,
                                 const char *chunk, size_t chunk_len, bool is_final,
                                 bb_serialize_json_span_t span_provenance);
} bb_serialize_json_ingest_t;

// Opaque streaming-scanner state -- same "cast the opaque _state[] to the
// real internal struct" pattern as bb_release_manifest_stream_ctx_t. Sized
// to hold the internal grammar/depth state machine plus the shared
// key/number/literal reassembly scratch at its Kconfig-configured maximum
// (CONFIG_BB_SERIALIZE_JSON_SCRATCH_MAX_BYTES, Kconfig range 16..256) PLUS
// headroom for future struct growth: at the Kconfig maximum of 256,
// sizeof(scan_state_t) is 328 on a 64-bit host build -- this value must
// stay comfortably above that, not shaved down to exactly what fits today.
// A compile-time _Static_assert in the .c file catches this ever falling
// short (verified by building at both Kconfig range endpoints, 16 and 256).
#define BB_SERIALIZE_JSON_SCAN_STATE_SIZE 384

typedef struct bb_serialize_json_scan_ctx bb_serialize_json_scan_ctx_t;
struct bb_serialize_json_scan_ctx {
    char _state[BB_SERIALIZE_JSON_SCAN_STATE_SIZE];
};

// Bounded entry point: scans a complete JSON document already held in a
// caller-owned, stable buffer. Because the whole document is present up
// front, no token can ever straddle a "feed" boundary -- keys/numbers/
// literals still reassemble through the internal scratch (same as the
// streaming path), but a string value that contains no escape sequences is
// delivered in exactly ONE value_str_chunk call whose span points directly
// into `buf` (is_final=true, span_provenance=CALLER_STABLE), never copied
// through scratch. A string that DOES contain an escape sequence decodes
// through the same run+escape mechanism as the streaming path (see
// bb_serialize_json_scan_feed) and may arrive across more than one call --
// the "sink never sees a naked backslash" guarantee always holds; only the
// single-call/direct-span optimization is specific to the escape-free case.
// IMPORTANT: a string consisting of a single escape immediately before the
// closing quote (e.g. `"\n"`) ALSO collapses to exactly one call with
// is_final=true, but its span is decoded-escape scratch, not a slice of
// `buf` -- span_provenance=SCANNER_SCRATCH on that call. Do not use call
// count/is_final as a proxy for span safety; see bb_serialize_json_span_t's
// contract on bb_serialize_json_ingest_t.value_str_chunk above.
bb_err_t bb_serialize_json_scan_bounded(const char *buf, size_t len,
                                         const bb_serialize_json_ingest_t *sink);

// Streaming entry point: composes with bb_http_client_chunk_cb. A string
// value MAY arrive as multiple value_str_chunk calls -- once per contiguous
// run of unescaped bytes, plus one call per decoded escape sequence, plus a
// final (possibly zero-length) call marking is_final=true. An escape
// sequence that straddles a bb_serialize_json_scan_feed() boundary (a lone
// `\`, a partially-received `\uXXXX`, or a `\uXXXX\uXXXX` surrogate pair
// split mid-pair) resumes correctly on the next feed() call; the sink is
// never shown the undecoded bytes.
//
// _begin initializes `ctx` and binds `sink` (borrowed; must outlive the
// scan). _feed may be called with any chunk size, including zero-length or
// single-byte chunks. _end finalizes the scan and must be called exactly
// once after the last _feed.
//
// Returns (from _feed and _end):
//   BB_OK                 -- continue (from _feed); scan complete (from _end)
//   BB_ERR_VALIDATION     -- malformed grammar: bad token/number/literal, an
//                             unescaped control character, or an
//                             unpaired/invalid `\uXXXX` surrogate
//   BB_ERR_INVALID_STATE  -- _end() called with an open container, an open
//                             string, or an incomplete number/literal
//   BB_ERR_NO_SPACE       -- BB_SERIALIZE_MAX_DEPTH nesting exceeded, or the
//                             shared key/number scratch overflowed
//   any other bb_err_t    -- propagated verbatim from the sink's own return
// Once any call returns a non-BB_OK code, the scanner is done: that code is
// returned sticky by every subsequent _feed/_end call without further
// processing.
bb_err_t bb_serialize_json_scan_begin(bb_serialize_json_scan_ctx_t *ctx,
                                       const bb_serialize_json_ingest_t *sink);
bb_err_t bb_serialize_json_scan_feed (bb_serialize_json_scan_ctx_t *ctx,
                                       const char *chunk, size_t chunk_len);
bb_err_t bb_serialize_json_scan_end  (bb_serialize_json_scan_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
