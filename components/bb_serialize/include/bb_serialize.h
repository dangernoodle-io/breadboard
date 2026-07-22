#pragma once

/**
 * @brief Format-neutral snapshot serialization: a descriptor SSOT + a pure
 * walker + the bb_serialize_emit_t emit-vtable seam.
 *
 * A consumer declares a static const bb_serialize_field_t table describing
 * a plain-old-data snapshot struct's fields (type, byte offset, optional
 * presence predicate, optional nested-object/array children), then calls
 * bb_serialize_walk() with any implementation of bb_serialize_emit_t.
 *
 * bb_serialize_emit_t is the swap seam: it carries a format_id but has no
 * built-in notion of a wire format. A format backend (JSON, msgpack, ...)
 * implements bb_serialize_emit_t in its OWN component (REQUIRES
 * bb_serialize) and is composed in by a consumer that wants it --
 * bb_serialize itself ships no format backend and has no JSON/cJSON/format
 * dependency of any kind. This PR ships only the core (descriptor + walker
 * + vtable interface); a JSON backend is a separate, later PR once its
 * implementation (hand-rolled vs an existing JSON library) is decided.
 *
 * The walker is pure: no heap, no locks, no I/O, no format knowledge. Field
 * array order is emit order (fidelity-load-bearing for wire-format
 * consumers that assert byte-exact output).
 *
 * bb_serialize_populate() is the pure INVERSE: same descriptor SSOT, driven
 * against a pull-based bb_serialize_populate_t source instead, scattering
 * fields INTO a snapshot struct rather than reading them out of one. See
 * the "Populate" section below for the full contract.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bb_core.h"
#include "bb_format.h"
#include "bb_type.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Request-scoped query-param carrier -- a neutral, HTTP-agnostic filter
// argument a caller (e.g. bb_data_render()) can thread down to a producer's
// gather/fill hook. Plain data -- no httpd types anywhere in this. Lives
// here (not bb_data) so a filtered producer (e.g. bb_storage's diag fill fn)
// can depend on bb_serialize alone and stay bb_data-free.
// ---------------------------------------------------------------------------

#define BB_SERIALIZE_QUERY_MAX_PARAMS 4  // fixed cap, no heap

typedef struct {
    const char *key;
    const char *value;
} bb_serialize_query_param_t;

typedef struct {
    bb_serialize_query_param_t params[BB_SERIALIZE_QUERY_MAX_PARAMS];
    size_t                     count;
} bb_serialize_query_t;

// Looks up `key` in `q`'s params, first match wins. NULL-safe: `q == NULL`
// or no match returns NULL (never BB_ERR_*, this is a plain lookup helper).
const char *bb_serialize_query_get(const bb_serialize_query_t *q, const char *key);

// ---------------------------------------------------------------------------
// Runtime carriers -- the value found AT a field's offset for indirect types
// ---------------------------------------------------------------------------

// Pointer+length pair for BB_TYPE_STR_N fields (non-NUL-terminated or
// bounded string content, e.g. a fixed-capacity buffer with a separate
// length rather than a NUL sentinel).
typedef struct {
    const char *ptr;
    size_t      len;
} bb_serialize_str_n_t;

// Generic array runtime carrier, located at a BB_TYPE_ARR field's offset.
// `items` is the base of contiguous element storage; `count` is the
// element count (0 = empty array -> emits begin_arr/end_arr only).
typedef struct {
    const void *items;
    size_t      count;
} bb_serialize_arr_t;

// Layout-compatible alias for a BB_TYPE_ARR field whose elem_type is
// BB_TYPE_STR -- `items` is reinterpreted as `const char *const *` by the
// walker in that case.
typedef bb_serialize_arr_t bb_serialize_arr_str_t;

// ---------------------------------------------------------------------------
// Streamed-array carrier -- the BB_ARR_STREAM alternative to
// bb_serialize_arr_t. A BB_TYPE_ARR field whose descriptor sets
// `.cardinality = BB_ARR_STREAM` carries this struct at its offset instead:
// a pull iterator over rows, rather than a pre-materialized contiguous
// array. Lets a producer with an unbounded/unknown-at-registration-time row
// count (e.g. live NVS inventory) stream elements one at a time -- no fixed
// caller-side capacity, no silent truncation. `cardinality` defaults to 0
// (BB_ARR_FIXED) for every existing descriptor that never sets it, so the
// walker's original arr.items/arr.count path stays byte-identical unless a
// descriptor explicitly opts in.
// ---------------------------------------------------------------------------

typedef enum {
    BB_ARR_FIXED  = 0,  // default -- bb_serialize_arr_t carrier (existing behavior)
    BB_ARR_STREAM = 1,  // bb_serialize_arr_stream_t carrier -- pull iterator
} bb_serialize_arr_cardinality_t;

// One row -> `row_out` (a caller-owned buffer of at least `row_size` bytes,
// reused per row -- the walker never keeps more than one row's bytes alive
// at a time). Returns true if a row was written, false once the source is
// exhausted (no further calls follow a false return for the same walk).
//
// MUST return false in bounded time (i.e. terminate after a finite number
// of calls for any one walk) -- the walker's `while (carrier.next(...))`
// loop (bb_serialize_walk.c) has NO row-count cap of its own. This is a
// deliberate no-caps-fights-the-design-principle choice, not an oversight:
// the only shipped wiring is bb_serialize_arr_stream_from_buf()'s iterator,
// which is inherently bounded by its backing buffer's `count`, `row_out`'s
// stack footprint is O(1) per row, and a JSON emit backend streams (no
// unbounded heap growth) -- so a non-terminating `next` is an ITERATOR
// LIVENESS bug in the callback, never a walker memory-safety issue. A
// future non-arena-backed iterator producer (one without an inherent
// backing-store bound) should supply its own termination bound internally.
typedef bool (*bb_serialize_iter_next_fn)(void *iter_ctx, void *row_out);

// Streamed-array runtime carrier, located at a BB_TYPE_ARR field's offset
// when `.cardinality == BB_ARR_STREAM`. `row_size` is defensive assert-only
// (expected to equal the field's own `elem_size`) -- not consulted by the
// walker itself.
typedef struct {
    bb_serialize_iter_next_fn next;
    void                     *iter_ctx;
    size_t                    row_size;
} bb_serialize_arr_stream_t;

// Walker-safety constant: the largest single row a BB_ARR_STREAM field may
// produce, sized generously above any real diag-section row shape. Fixed
// #define (like BB_SERIALIZE_MAX_DEPTH below), not a Kconfig knob -- it
// bounds a stack buffer the walker itself allocates per STREAM field.
#define BB_SERIALIZE_MAX_ROW_BYTES 256

// Generic, pure, no-heap BB_ARR_STREAM iterator over a caller-owned flat
// buffer -- reusable by every iter section rather than each hand-rolling
// its own next() over a contiguous array. `state` is caller-owned storage
// that MUST outlive the walk (typically a member of the snapshot struct
// itself, since the snapshot lives across the walk/render call).
typedef struct {
    const uint8_t *base;
    size_t         count;
    size_t         elem_size;
    size_t         idx;
} bb_serialize_arr_buf_iter_t;

// Wires `state` to iterate `buf` (an array of `count` elements, each
// `elem_size` bytes) and returns the resulting carrier. Pure, no heap --
// `next()` returns false once `idx >= count` (never dereferences `buf` in
// that case, so `buf == NULL, count == 0` is safe and yields an
// immediately-exhausted iterator).
bb_serialize_arr_stream_t bb_serialize_arr_stream_from_buf(bb_serialize_arr_buf_iter_t *state,
                                                             const void *buf, size_t count,
                                                             size_t elem_size);

// Optional per-field presence predicate. NULL means always-present. Lets a
// field be omitted conditionally (e.g. a zero/unset timestamp) without a
// distinct field type.
typedef bool (*bb_serialize_present_fn)(const void *snap);

// ---------------------------------------------------------------------------
// Field + descriptor tables -- caller-owned, typically `static const`
// ---------------------------------------------------------------------------

// Maximum BB_TYPE_OBJ / BB_TYPE_ARR-of-OBJ nesting depth the walker will
// descend into. Defensive against a copy-paste circular `children` pointer
// silently stack-overflowing; caller data is static-const/trusted, so this
// guard is cheap defense-in-depth, not a real-world limit -- a legitimate
// snapshot table is nowhere near this deep.
#define BB_SERIALIZE_MAX_DEPTH 8

// Rich field descriptor. `offset` is the byte offset of the field's value
// (or of its bb_serialize_arr_t carrier, for BB_TYPE_ARR) within the
// snapshot struct passed to bb_serialize_walk(). `children` / `n_children`
// apply to BB_TYPE_OBJ (the nested struct found at `offset`) and to
// BB_TYPE_ARR with elem_type == BB_TYPE_OBJ (each array element's fields).
typedef struct bb_serialize_field_s {
    const char                        *key;
    bb_type_t                          type;
    uint16_t                           offset;
    bb_serialize_present_fn            present;
    const struct bb_serialize_field_s *children;   // OBJ fields, OR ARR element-obj fields
    uint16_t                           n_children;

    // BB_TYPE_STR: the embedded char[N] buffer's array capacity at
    // `offset` (e.g. 16 for `char status[16]`). The walker reads the string
    // via strnlen(p, max_len) -- NEVER strlen -- so a buffer that isn't
    // NUL-terminated within its bound (network-filled/corrupt/racy
    // snapshot) can never be read past its own array end; it yields exactly
    // max_len bytes instead of running off the end. MUST be set for every
    // BB_TYPE_STR field to a nonzero value -- an unset (0) max_len is safe
    // (strnlen(p, 0) yields an empty string) but silently wrong.
    //
    // BB_TYPE_ARR with elem_type == BB_TYPE_STR: same contract, applied to
    // EACH element -- max_len bounds strnlen() of every `const char *`
    // element read from `items`, never a raw strlen. Every element must be
    // NUL-terminated within max_len.
    //
    // Reserved (not consumed by the walker) for all other types -- an
    // upper-bound hint for a future byte-bound helper (e.g.
    // bb_serialize_json_bound()) that needs worst-case per-field width
    // metadata. Leave 0 unless such a helper documents otherwise.
    uint16_t                           max_len;

    // Upper-bound hint (item count). For bb_serialize_walk(), reserved
    // only -- not consumed (bb_serialize_arr_t.count is authoritative at
    // walk time); a future byte-bound helper (e.g. bb_serialize_json_bound())
    // treats 0 as unknown/unbounded.
    //
    // For bb_serialize_populate(), max_items is instead CONSUMED as the
    // destination array's writable CAPACITY, and diverges from that
    // 0-means-unbounded convention: populate scatters into caller-prewired,
    // fixed-size storage, so "unbounded" has no meaning there -- there is
    // no destination buffer to size against. max_items MUST be > 0 for
    // every BB_TYPE_ARR field in a descriptor passed to
    // bb_serialize_populate(); a max_items == 0 array field is rejected as
    // BB_ERR_INVALID_ARG (see bb_serialize_populate()'s doc) rather than
    // silently degrading to a zero-element array.
    uint16_t                           max_items;

    // BB_TYPE_ARR only: the element shape -- BB_TYPE_STR (items is
    // `const char *const *`) or BB_TYPE_OBJ (items is a contiguous array of
    // element structs described by `children`/`n_children`, each
    // `elem_size` bytes apart). Unused for all other types.
    bb_type_t                          elem_type;

    // BB_TYPE_ARR + elem_type == BB_TYPE_OBJ only: the element stride, i.e.
    // sizeof(the element struct). Unused for all other types.
    uint16_t                           elem_size;

    // BB_TYPE_ARR only: which runtime carrier lives at `offset` --
    // bb_serialize_arr_t (BB_ARR_FIXED, the zero-init default) or
    // bb_serialize_arr_stream_t (BB_ARR_STREAM). Every existing descriptor
    // leaves this unset (0 == BB_ARR_FIXED), so the walker's original
    // FIXED-array path is unaffected unless a descriptor explicitly opts
    // into BB_ARR_STREAM. Unused for all other types. BB_ARR_STREAM is
    // rejected by bb_serialize_populate() (BB_ERR_UNSUPPORTED, same
    // precedent as STR_N/REF) and bb_serialize_json_bound() (SIZE_MAX,
    // unbounded) -- neither has a scatter/sizing story for a pull iterator.
    bb_serialize_arr_cardinality_t     cardinality;

    // BB_TYPE_REF only: the REGISTRY lookup key passed to a
    // bb_serialize_ref_resolve_fn to find the referenced sibling's
    // descriptor+snapshot. Deliberately distinct from `.key` -- `.key` is
    // the WIRE key this field is emitted under; `.ref_key` is an internal
    // cache/registry namespace key, which need not (and often won't) match
    // the wire schema. Unused for all other types.
    const char                        *ref_key;
} bb_serialize_field_t;

// Descriptor for one snapshot struct type -- the SSOT a walker/backend pair
// consumes. `fields` order is emit order.
typedef struct {
    const char                 *type_name;
    const bb_serialize_field_t *fields;
    uint16_t                    n_fields;
    uint16_t                    snap_size;
} bb_serialize_desc_t;

// ---------------------------------------------------------------------------
// Emit vtable -- THE format-neutral swap seam
// ---------------------------------------------------------------------------
//
// `format_id` identifies the wire format this vtable implementation
// ultimately produces (BB_FORMAT_NONE for a non-wire consumer, e.g. a test
// recording mock) -- reserved for a future (format, key, version)
// render-memo; bb_serialize_walk() itself never inspects it.
//
// Every callback receives `ctx` (== this vtable's own `ctx` member, set up
// by the backend that populates the vtable). `key` is NULL for an array
// element or the walked root; non-NULL for an object member.
//
// Presence/nullness contract: a field whose `present` predicate returns
// false is OMITTED entirely -- no emit_* call of any kind. A field that IS
// present but genuinely has no value -- a BB_TYPE_STR_N whose `.ptr` is
// NULL, or a NULL item within a BB_TYPE_ARR of BB_TYPE_STR -- drives
// `emit_null`, distinct from an empty string (emit_str with len 0, e.g. a
// non-NULL BB_TYPE_STR_N with .len == 0). A JSON backend renders the two
// differently: `null` vs `""`.
//
// A format backend (e.g. a JSON writer, in its own component) implements
// this vtable and bb_serialize_walk() drives it -- bb_serialize itself has
// no knowledge of what format the callbacks ultimately produce, and ships
// no backend of its own.
typedef struct {
    bb_format_t format_id;
    void       *ctx;

    void (*begin_obj)(void *ctx, const char *key);
    void (*end_obj)  (void *ctx);
    void (*begin_arr)(void *ctx, const char *key);
    void (*end_arr)  (void *ctx);

    void (*emit_i64) (void *ctx, const char *key, int64_t v);
    void (*emit_u64) (void *ctx, const char *key, uint64_t v);
    void (*emit_f64) (void *ctx, const char *key, double v);
    void (*emit_bool)(void *ctx, const char *key, bool v);
    void (*emit_str) (void *ctx, const char *key, const char *s, size_t len);
    void (*emit_null)(void *ctx, const char *key);
} bb_serialize_emit_t;

// ---------------------------------------------------------------------------
// REF resolver seam -- BB_TYPE_REF fields
// ---------------------------------------------------------------------------
//
// A BB_TYPE_REF field composes a sibling snapshot's fields inline at this
// field's wire key, without the parent snapshot struct embedding the
// sibling's data directly. `resolve` maps a field's `.ref_key` to the
// sibling's own descriptor+snapshot; the walker then recurses into that
// descriptor exactly as it would a BB_TYPE_OBJ child (same depth-cap
// accounting, same one-shot per-field cost).

// A resolved sibling: `desc`/`snap` describe and locate the referenced
// section's own fields, structurally independent of the REF field's parent
// snapshot.
typedef struct {
    const bb_serialize_desc_t *desc;  // sibling's descriptor
    const void                *snap;  // sibling's live snapshot -- BORROWED pointer,
                                       // valid only for the duration of the REF subtree's
                                       // emit calls; same single-threaded-consumer contract
                                       // as bb_cache_serialize_get's memo-slot pointers.
} bb_serialize_ref_t;

// Resolver callback: given a BB_TYPE_REF field's `.ref_key` and the
// caller-supplied `ctx`, fills `*out` and returns true if the sibling
// exists; returns false (leaving `*out` untouched) if it does not -- the
// walker treats a false return identically to a NULL resolver, i.e. the REF
// field is omitted entirely.
typedef bool (*bb_serialize_ref_resolve_fn)(const char *ref_key, void *ctx,
                                             bb_serialize_ref_t *out);

// ---------------------------------------------------------------------------
// The pure walker
// ---------------------------------------------------------------------------

// Walks desc->fields against snap (the snapshot struct's base address),
// driving emit for each present field in table order. No heap, no locks,
// no I/O, no format knowledge -- purely a descriptor interpreter. Does NOT
// wrap the walked fields in a begin_obj/end_obj pair itself; a backend's
// one-shot entry point (e.g. bb_serialize_json()) does that around the
// call if the wire format wants a root container.
//
// Thin wrapper over bb_serialize_walk_ref(desc, snap, emit, NULL, NULL) --
// zero behavior/signature change for every existing caller. A descriptor
// containing a BB_TYPE_REF field omits it (no resolver == unresolved
// sibling; see bb_serialize_walk_ref()).
void bb_serialize_walk(const bb_serialize_desc_t *desc, const void *snap,
                        const bb_serialize_emit_t *emit);

// Same as bb_serialize_walk(), plus REF resolution: any BB_TYPE_REF field
// encountered calls `resolve(field->ref_key, resolve_ctx, &ref)`; on true,
// the sibling's fields (ref.desc/ref.snap) are walked inline at the REF
// field's wire key (`.key`), exactly like a BB_TYPE_OBJ child -- same
// begin_obj/end_obj bracketing, same depth-cap accounting (a REF hop costs
// one depth level, same as OBJ). On false, or when `resolve` is NULL, the
// field is OMITTED entirely (no begin_obj/end_obj, no emit_null) -- the
// same convention as a `present`-false field. If the REF field also
// carries a `.present` predicate, it is evaluated FIRST; present-false
// short-circuits before resolution is ever attempted.
void bb_serialize_walk_ref(const bb_serialize_desc_t *desc, const void *snap,
                            const bb_serialize_emit_t *emit,
                            bb_serialize_ref_resolve_fn resolve, void *resolve_ctx);

// Top-level field lookup by key (not recursive into BB_TYPE_OBJ children)
// -- e.g. for a future direct-read display path. Returns NULL if desc,
// key, or a matching field is absent.
const bb_serialize_field_t *bb_serialize_desc_find(const bb_serialize_desc_t *desc,
                                                    const char *key);

// ---------------------------------------------------------------------------
// Populate vtable -- THE pull-based inverse of bb_serialize_emit_t, and the
// pure inverse walker driving it
// ---------------------------------------------------------------------------
//
// A format backend (JSON parser, msgpack, ...) implements this vtable in
// its OWN component (REQUIRES bb_serialize) and bb_serialize_populate()
// drives it against the SAME descriptor a walk() call would emit from --
// same SSOT, opposite direction. bb_serialize itself ships no format
// backend and has no JSON/cJSON/format dependency of any kind.
//
// Every callback receives `ctx` (== this vtable's own `ctx` member). `key`
// is NULL for an array element getter/container, matching emit's
// null-for-array-element convention.
//
// Presence contract: a getter/container callback returning false means the
// field is ABSENT in the source -- populate leaves the corresponding
// destination bytes entirely UNTOUCHED (never zeroes them). Callers MUST
// pass a zero-initialized `dst` so an absent field keeps its zero/default
// value; populate never zeroes `dst` itself.
//
// field->present is NOT consulted by populate. That gate is emit-direction
// only -- it inspects the very struct being walked OUT of, which at
// populate time is the struct still being filled IN, so evaluating it here
// would race the fields it's meant to gate. Presence is instead driven
// entirely by each callback's own bool return.
//
// Duplicate `.key` on the populate/ingress path IS SUPPORTED: two (or more)
// fields in the same table MAY share a `.key`, each with a different
// `.type`/`.offset` -- e.g. one field reading a wire number as BB_TYPE_U64
// and a second reading the SAME wire number as BB_TYPE_F64, to compare two
// independent conversions of the identical source value. This works
// because every scalar/string getter above does a STATELESS, independent
// first-match lookup of its own `.key` against the source on each call --
// no shared cursor state carries between fields, so field order within the
// table is irrelevant to which occurrence of a duplicated key each field
// resolves (there is only ever one occurrence to find on the wire; the
// duplication is in the DESCRIPTOR, not the source document). This is a
// property of every current populate backend (see e.g.
// bb_serialize_json_populate.c's per-callback bb_serialize_json_tok_obj_
// get() calls), not something bb_serialize_populate() itself special-
// cases -- populate_check_fields() neither allows nor rejects a duplicate
// key by name, it simply never inspects `.key` for uniqueness. A future
// change to a single forward-only cursor pass, or duplicate-key rejection
// added to populate_check_fields(), would silently break this pattern --
// see platform/espidf/bb_system/bb_system_routes.c's s_reboot_fields[] for
// a real consumer relying on it.
//
// get_str contract: `cap` is the field's `max_len`; the getter owns
// bounds-checking a value into that capacity (the same bounded-write
// convention as an embedded char[N] snapshot field) -- populate trusts a
// typed getter rather than re-validating what it writes. Unlike the scalar
// getters (which write into a local scratch temp and only memcpy it into
// `dst` on a true return), get_str writes directly into `dst` -- so a
// get_str implementation returning false MUST NOT have written to `dst`
// first; populate has no scratch temp to fall back on for STR and relies
// entirely on the getter honoring this contract.
//
// BB_TYPE_STR_N and BB_TYPE_REF are NOT supported by populate -- both
// target caller-owned storage that lives outside the snapshot struct (a
// STR_N's `.ptr`, a REF's resolved sibling) with no settled scatter
// convention yet. Unlike an absent field, a descriptor containing either is
// rejected LOUD: bb_serialize_populate() pre-flight-scans the whole
// descriptor tree before writing anything and returns BB_ERR_UNSUPPORTED if
// it finds one, rather than silently leaving the destination under-
// populated. A future PR can add explicit support once that storage story
// is designed.
//
// Array contract (BB_TYPE_ARR): populate never allocates. The destination
// bb_serialize_arr_t at the field's offset must already have `.items`
// pre-wired by the caller to writable, contiguous storage -- elem_size-
// strided element structs for elem_type == BB_TYPE_OBJ, or an array of
// writable `char *` buffers (each capacity >= `max_len`) for elem_type ==
// BB_TYPE_STR. `max_items` is the consumed destination CAPACITY bound here
// (see the field doc above) and MUST be > 0 -- a max_items == 0 array
// field fails the pre-flight check with BB_ERR_INVALID_ARG before any
// scatter begins. Otherwise populate reads at most min(source count,
// max_items) elements and never writes past that pre-wired storage; the
// source's begin_arr() call reports its own count via `*count`, purely
// informational for the array's actual bound. On return,
// the destination's `.count` reflects how many elements were actually
// written (fewer than requested if a source getter returns false
// mid-array, in which case the loop stops early rather than leaving a
// hole followed by more writes).
typedef struct {
    bb_format_t format_id;
    void       *ctx;

    bool (*get_i64) (void *ctx, const char *key, int64_t  *out);
    bool (*get_u64) (void *ctx, const char *key, uint64_t *out);
    bool (*get_f64) (void *ctx, const char *key, double   *out);
    bool (*get_bool)(void *ctx, const char *key, bool     *out);
    bool (*get_str) (void *ctx, const char *key, char *dst, size_t cap, size_t *out_len);

    bool (*begin_obj)(void *ctx, const char *key);
    bool (*end_obj)  (void *ctx);
    bool (*begin_arr)(void *ctx, const char *key, size_t *count);
    bool (*end_arr)  (void *ctx);
} bb_serialize_populate_t;

// Walks desc->fields, scattering each present field's value from `src` into
// `dst` (the destination snapshot struct's base address) in table order.
// No heap, no locks, no I/O, no format knowledge -- purely a descriptor
// interpreter, the pull-direction mirror of bb_serialize_walk(). Bounded
// recursion at BB_SERIALIZE_MAX_DEPTH -- unlike bb_serialize_walk() (which
// silently truncates a runaway descriptor), populate returns BB_ERR_NO_SPACE
// rather than proceeding once the depth budget is exhausted, since a
// partial-but-silent scatter into caller memory is a worse failure mode
// than a walker's read-only truncation.
//
// Before scattering anything, pre-flight-scans the whole descriptor tree
// (same depth cap): a BB_TYPE_ARR field with max_items == 0 fails with
// BB_ERR_INVALID_ARG (see the field doc above -- populate's capacity bound
// must be nonzero, unlike JSON's 0-means-unbounded), a BB_TYPE_STR_N or
// BB_TYPE_REF field anywhere in the tree fails with BB_ERR_UNSUPPORTED
// (neither is scatter-supported yet -- see the populate vtable doc above),
// and an OBJ or ARR field sitting at nesting depth >= BB_SERIALIZE_MAX_DEPTH
// fails with BB_ERR_NO_SPACE (either type costs the source a container
// frame at that depth, which the runtime depth guard below would otherwise
// only catch mid-scatter). Depth is purely structural -- a function of the
// descriptor tree, never of source data -- so this scan is a sound
// predictor of every depth bb_serialize_populate() could ever reach for the
// same descriptor: a depth violation always fails here, before any scatter
// begins, never mid-scatter. This keeps a misconfigured/unsupported/
// too-deep descriptor from ever reaching a partial write. When a field
// violates multiple constraints simultaneously (e.g., misconfigured and
// too-deep), the depth check runs first and BB_ERR_NO_SPACE is reported.
//
// Returns BB_ERR_INVALID_ARG if desc, dst, or src is NULL, or if the
// pre-flight scan above rejects a max_items == 0 array field;
// BB_ERR_UNSUPPORTED if the pre-flight scan finds a STR_N/REF field;
// BB_ERR_NO_SPACE if the pre-flight scan finds an OBJ/ARR field at or past
// the depth cap; BB_OK otherwise (an individual absent field is not an
// error -- see the presence contract above). All three rejections above run
// before any scatter, so `dst` stays fully untouched on any non-BB_OK
// return.
bb_err_t bb_serialize_populate(const bb_serialize_desc_t *desc, void *dst,
                                const bb_serialize_populate_t *src);

#ifdef __cplusplus
}
#endif
