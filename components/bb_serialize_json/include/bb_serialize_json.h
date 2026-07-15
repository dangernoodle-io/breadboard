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

// Registers this backend under BB_FORMAT_JSON in bb_serialize's
// format-dispatch registry (bb_serialize_format_register(), see
// bb_serialize_format.h) so a runtime consumer can look up the JSON emit
// vtable/scanner by bb_format_t rather than #include-ing this header
// directly. The registered emit vtable's `ctx` is NULL (a template -- see
// bb_serialize_json_emit()'s own contract); a caller that looks it up via
// bb_serialize_format_get_emit() must copy the vtable and set its own `ctx`
// before driving bb_serialize_walk(). The registered parse handle is
// bb_serialize_json_scan_bounded(), cast to `const void *` per
// bb_serialize_format_entry_t's opaque-parse contract -- a caller that
// looks it up via bb_serialize_format_get_parse() casts it back to
// `bb_err_t (*)(const char *, size_t, const bb_serialize_json_ingest_t *)`.
// Idempotent (last-writer-wins, per bb_serialize_format_register()) --
// safe to call more than once.
// bbtool:init tier=early fn=bb_serialize_json_register_format
bb_err_t bb_serialize_json_register_format(void);

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
// CONTRACT (BOUNDED mode only, i.e. bb_serialize_json_scan_bounded()): a
// `value_str_chunk` call's `chunk_len` is NEVER zero except for the
// empty-string case (is_final=true on the call, zero-length span). Every
// other call -- non-final, or final-but-non-empty -- always carries
// chunk_len > 0. A bounded-mode-only sink MAY rely on this to skip a
// `chunk_len == 0` guard on any call it has already distinguished from the
// empty-string case.
//
// STREAMING mode (bb_serialize_json_scan_feed()) does NOT get this
// guarantee: a zero-length, is_final=true call also occurs whenever a feed
// boundary falls between the last content byte and the closing quote --
// e.g. `{"x":"ab` fed in one chunk and `"}` in the next yields
// (chunk_len=2, is_final=false) followed by (chunk_len=0, is_final=true)
// for a non-empty string. A streaming sink MUST tolerate chunk_len == 0 on
// any call, final or not.
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

// ---------------------------------------------------------------------------
// Token recorder -- a bb_serialize_json_ingest_t sink that records a scanned
// document into a flat, caller-owned pool of tokens with random-access
// lookup (object-key/array-index navigation, scalar accessors). BOUNDED-MODE
// ONLY: bb_serialize_json_tok_recorder_init() takes the SAME `buf`/`len` the
// caller passes to bb_serialize_json_scan_bounded() -- this is deliberate,
// not incidental, and is the structural signal that this sink must never be
// wired to bb_serialize_json_scan_begin()/_feed()/_end(). The recorder also
// runtime-rejects (BB_ERR_INVALID_STATE) any value_str_chunk call carrying
// BB_SERIALIZE_JSON_SPAN_CALLER_FEED_SCOPED provenance -- a value that ONLY
// ever occurs in streaming mode -- as a defense-in-depth misuse guard, in
// case a caller ignores the structural signal and wires this sink to the
// streaming entry points anyway.
//
// KEY STORAGE: unlike string VALUES, object-member keys are NEVER a stable
// span of the caller's `buf` -- the scanner always fully reassembles a key
// into its own internal (callback-scoped) scratch buffer before invoking any
// callback (see bb_serialize_json_ingest_t above), regardless of scan mode.
// So every key the recorder wants to keep MUST be copied; rather than
// spending arena bytes on schema-known, small, bounded key names, each token
// carries a small INLINE key buffer (BB_SERIALIZE_JSON_TOK_KEY_MAX_LEN) --
// this is what lets an arena-free caller (e.g. Stratum, whose object keys
// are short but whose string VALUES are escape-free hex) pay zero arena
// bytes while object navigation still works.
//
// STRING VALUE STORAGE: an escape-free string value is delivered by
// bb_serialize_json_scan_bounded() in exactly ONE value_str_chunk call whose
// span is a direct, durable slice of `buf` (CALLER_STABLE) -- the recorder
// detects this case (is_final on the very first call, CALLER_STABLE
// provenance) and records it as a zero-copy (ptr, len) directly into `buf`.
// Any string value that needs MORE than that one call (i.e. it contains at
// least one escape sequence, per the bounded-mode contract) has chunks that
// ALTERNATE between direct spans of `buf` and scanner-owned decoded-escape
// scratch that dies at the end of each callback -- there is no single
// offset span in `buf` that represents the assembled value. For that case
// the recorder copies EVERY chunk of the string (direct-span runs and
// decoded-escape scratch alike) into the caller-supplied `arena`, appending
// contiguously, and records the assembled (ptr, len) pointing into `arena`.
// If `arena` is NULL or the copy would overflow `arena_cap`, the callback
// returns BB_ERR_NO_SPACE -- the scan aborts cleanly (per
// bb_serialize_json_ingest_t's contract) and no dangling/truncated pointer
// is ever recorded. `bb_serialize_json_tok_get_str()` returns the token's
// (ptr, len) transparently -- callers never need to know whether a given
// string landed in `buf` or `arena`.
// ---------------------------------------------------------------------------

typedef enum {
    BB_SERIALIZE_JSON_TOK_OBJ,
    BB_SERIALIZE_JSON_TOK_ARR,
    BB_SERIALIZE_JSON_TOK_STR,
    BB_SERIALIZE_JSON_TOK_NUM,
    BB_SERIALIZE_JSON_TOK_BOOL,
    BB_SERIALIZE_JSON_TOK_NULL,
} bb_serialize_json_tok_type_t;

// Signed token-pool index; BB_SERIALIZE_JSON_TOK_ABSENT is the sentinel
// every lookup/navigation function returns for "not found" -- distinct from
// a valid index 0..pool_n-1, including index 0 (the root token). Every
// scalar accessor is a safe no-op (returns false, leaves *out untouched)
// when passed this sentinel, so a caller never needs to guard a lookup
// before reading it -- this is how an OPTIONAL positional element (e.g.
// Stratum's `clean_jobs`) is distinguished from a present-but-false one:
// absent -> lookup returns BB_SERIALIZE_JSON_TOK_ABSENT, get_bool() on it
// returns false (not-found) and never touches *out; present-and-false ->
// lookup returns a real index, get_bool() returns true (found) with
// *out == false.
typedef int32_t bb_serialize_json_tok_idx_t;
#define BB_SERIALIZE_JSON_TOK_ABSENT ((bb_serialize_json_tok_idx_t)-1)

// Inline key-name capacity: covers every key seen across the Stratum
// message set with headroom, e.g. "version-rolling.mask" (20 bytes) --
// see bb_serialize_json_tok_recorder_init()'s KEY STORAGE note above for why
// this is inline rather than arena-backed. A key longer than this fails the
// scan with BB_ERR_NO_SPACE (same as any other pool/arena exhaustion), never
// truncated silently.
#define BB_SERIALIZE_JSON_TOK_KEY_MAX_LEN 24

// One recorded token. Deliberately flat/small -- pool is a caller-owned
// static/stack array, not heap:
//   - type/key_len: 1 byte each (type is a small enum; key_len fits
//     BB_SERIALIZE_JSON_TOK_KEY_MAX_LEN in one byte with room to spare).
//   - child_count: uint16_t -- OBJ/ARR direct-child count; JSON grammar
//     bounds a single container's member/element count far below 65536 for
//     any document this recorder's pool could hold (a 65536-element array
//     would itself exhaust any realistic pool_cap long before the counter
//     could).
//   - parent: bb_serialize_json_tok_idx_t (int32_t) -- matches the
//     navigation index type; BB_SERIALIZE_JSON_TOK_ABSENT for the root.
//   - key[]: BB_SERIALIZE_JSON_TOK_KEY_MAX_LEN inline bytes, always safe to
//     read regardless of arena presence (see the KEY STORAGE note above).
//   - v: a union of the STR span (ptr into `buf` or `arena`, plus a 32-bit
//     length -- more than sufficient for any bounded in-RAM document this
//     recorder could realistically scan), the parsed NUM (both int64_t and
//     double, so get_i64()/get_f64() are both O(1) with no re-parse), and
//     the BOOL payload. OBJ/ARR/NULL tokens carry no payload.
// sizeof(bb_serialize_json_tok_t) is 48 bytes (8 header bytes + 24 key
// bytes + a 16-byte union, no padding beyond that at 8-byte alignment) --
// verified equal on both a 64-bit host build and 32-bit ARM by a
// _Static_assert in bb_serialize_json_tok.c. At the Kconfig-suggested
// default pool capacity (BB_SERIALIZE_JSON_TOK_POOL_DEFAULT_CAP, 48) that's
// 48*48 = 2304 bytes (~2.25KB) for a caller-owned pool sized to the default
// -- see BB_SERIALIZE_JSON_TOK_POOL_DEFAULT_CAP's doc comment below for the
// exact worst-case Stratum token-count arithmetic that default is sized
// against.
typedef struct {
    uint8_t                     type;       // bb_serialize_json_tok_type_t
    uint8_t                     key_len;    // 0 => no key (array elem / root)
    uint16_t                    child_count; // OBJ/ARR: direct-child count
    bb_serialize_json_tok_idx_t parent;      // BB_SERIALIZE_JSON_TOK_ABSENT for root
    char                        key[BB_SERIALIZE_JSON_TOK_KEY_MAX_LEN];
    union {
        struct { const char *ptr; uint32_t len; } str;  // TOK_STR
        struct { int64_t i64; double f64; }        num;  // TOK_NUM
        bool                                       b;    // TOK_BOOL
    } v;
} bb_serialize_json_tok_t;

// ---------------------------------------------------------------------------
// Kconfig bridge -- CONFIG_BB_SERIALIZE_JSON_TOK_POOL_DEFAULT_CAP -> a C
// default. Never shadow the generated symbol with a bare #ifndef. This is a
// SUGGESTED default for callers sizing their own pool array (the actual
// pool/pool_cap are supplied explicitly to
// bb_serialize_json_tok_recorder_init() below) -- sized to comfortably
// exceed the worst-case Stratum mining.notify document: 16 merkle branches
// plus the optional clean_jobs element present. Token count for that
// document: 1 (root obj) + 1 (id) + 1 (method) + 1 (params arr) + 4
// (job_id/prevhash/coinb1/coinb2) + 1 (merkle_branch arr) + 16 (branch
// strings) + 3 (version/nbits/ntime) + 1 (clean_jobs) = 29 tokens.
// ---------------------------------------------------------------------------
#ifdef CONFIG_BB_SERIALIZE_JSON_TOK_POOL_DEFAULT_CAP
#define BB_SERIALIZE_JSON_TOK_POOL_DEFAULT_CAP CONFIG_BB_SERIALIZE_JSON_TOK_POOL_DEFAULT_CAP
#else
#define BB_SERIALIZE_JSON_TOK_POOL_DEFAULT_CAP 48
#endif

// Opaque-ish recorder state -- caller-owned, no heap. `pool`/`pool_cap` and
// `arena`/`arena_cap` are borrowed (must outlive the recorder); `arena` may
// be NULL/`arena_cap` may be 0 if the caller knows every string value in the
// documents it will scan is escape-free (e.g. Stratum's hex-only strings) --
// see the STRING VALUE STORAGE note above.
typedef struct {
    const char *buf;      // the SAME buf bb_serialize_json_scan_bounded() will scan
    size_t      buf_len;

    bb_serialize_json_tok_t *pool;
    size_t                   pool_cap;
    size_t                   pool_n;

    char  *arena;          // optional; NULL/0 if the caller has no escaped strings
    size_t arena_cap;
    size_t arena_used;

    bb_serialize_json_tok_idx_t stack[BB_SERIALIZE_MAX_DEPTH];
    uint8_t                     depth;

    bool   str_open;            // true while assembling a multi-call (arena) string
    bool   str_use_arena;
    bb_serialize_json_tok_idx_t str_tok_idx;
    size_t str_arena_start;
    size_t str_len;
} bb_serialize_json_tok_recorder_t;

// Initializes `rec` over a caller-owned `pool`/`pool_cap` token array and an
// OPTIONAL `arena`/`arena_cap` byte buffer (pass NULL/0 if every string
// value in scope is known escape-free). `buf`/`len` MUST be the exact same
// buffer/length the caller subsequently passes to
// bb_serialize_json_scan_bounded() -- see the BOUNDED-MODE ONLY note above.
// Returns BB_ERR_INVALID_ARG if `rec`, `buf`, or `pool` is NULL, or
// `pool_cap` is 0.
bb_err_t bb_serialize_json_tok_recorder_init(bb_serialize_json_tok_recorder_t *rec,
                                              const char *buf, size_t len,
                                              bb_serialize_json_tok_t *pool, size_t pool_cap,
                                              char *arena, size_t arena_cap);

// Returns a bb_serialize_json_ingest_t vtable bound to `rec`. Pass the
// result to bb_serialize_json_scan_bounded() ONLY -- see the BOUNDED-MODE
// ONLY note above.
bb_serialize_json_ingest_t bb_serialize_json_tok_recorder_ingest(bb_serialize_json_tok_recorder_t *rec);

// Returns the root token's index (always 0 once a scan has recorded at
// least one token), or BB_SERIALIZE_JSON_TOK_ABSENT if `rec` is NULL or no
// token has been recorded yet (e.g. before the first scan, or after a scan
// that failed before producing a root token).
bb_serialize_json_tok_idx_t bb_serialize_json_tok_root(const bb_serialize_json_tok_recorder_t *rec);

// Predicates: each returns false (never asserts/crashes) for
// BB_SERIALIZE_JSON_TOK_ABSENT, an out-of-range index, or a token of a
// different type -- always a safe no-op.
bool bb_serialize_json_tok_is_obj (const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx);
bool bb_serialize_json_tok_is_arr (const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx);
bool bb_serialize_json_tok_is_str (const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx);
bool bb_serialize_json_tok_is_num (const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx);
bool bb_serialize_json_tok_is_bool(const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx);
bool bb_serialize_json_tok_is_null(const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx);

// Object-member lookup by key, scoped to `obj` (must be an OBJ token).
// Returns BB_SERIALIZE_JSON_TOK_ABSENT if `obj` is not a valid OBJ token or
// no member named `key` (key_len bytes, NOT NUL-terminated) exists.
bb_serialize_json_tok_idx_t bb_serialize_json_tok_obj_get(const bb_serialize_json_tok_recorder_t *rec,
                                                           bb_serialize_json_tok_idx_t obj,
                                                           const char *key, size_t key_len);

// Direct-child (element) count of `arr` (must be an ARR token). Returns -1
// if `arr` is not a valid ARR token.
int32_t bb_serialize_json_tok_arr_size(const bb_serialize_json_tok_recorder_t *rec,
                                        bb_serialize_json_tok_idx_t arr);

// Array-element lookup by position, scoped to `arr` (must be an ARR token).
// Returns BB_SERIALIZE_JSON_TOK_ABSENT if `arr` is not a valid ARR token or
// `elem_idx` is out of range (>= bb_serialize_json_tok_arr_size(rec, arr)).
bb_serialize_json_tok_idx_t bb_serialize_json_tok_arr_at(const bb_serialize_json_tok_recorder_t *rec,
                                                          bb_serialize_json_tok_idx_t arr, size_t elem_idx);

// Scalar accessors. Each returns true and writes `*out` (if non-NULL) when
// `idx` names a token of the matching type; returns false and leaves `*out`
// untouched otherwise (BB_SERIALIZE_JSON_TOK_ABSENT, out-of-range, or
// wrong-type token) -- always a safe no-op, no guard needed before the call.
// get_str's (ptr, len) is NEVER NUL-terminated -- see the STRING VALUE
// STORAGE note above for where `ptr` may point (`buf` or `arena`).
bool bb_serialize_json_tok_get_str (const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx,
                                     const char **out_ptr, size_t *out_len);
bool bb_serialize_json_tok_get_i64 (const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx,
                                     int64_t *out);
bool bb_serialize_json_tok_get_f64 (const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx,
                                     double *out);
bool bb_serialize_json_tok_get_bool(const bb_serialize_json_tok_recorder_t *rec, bb_serialize_json_tok_idx_t idx,
                                     bool *out);

#ifdef __cplusplus
}
#endif
