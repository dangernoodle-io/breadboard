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
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bb_format.h"
#include "bb_type.h"

#ifdef __cplusplus
extern "C" {
#endif

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

    // Reserved upper-bound hint (item count) for the same future byte-bound
    // helper; 0 means unknown. Not consumed by the walker
    // (bb_serialize_arr_t.count is authoritative at walk time) -- reserved
    // now so descriptor tables declared today don't need a second breaking
    // pass when the bound helper lands.
    uint16_t                           max_items;

    // BB_TYPE_ARR only: the element shape -- BB_TYPE_STR (items is
    // `const char *const *`) or BB_TYPE_OBJ (items is a contiguous array of
    // element structs described by `children`/`n_children`, each
    // `elem_size` bytes apart). Unused for all other types.
    bb_type_t                          elem_type;

    // BB_TYPE_ARR + elem_type == BB_TYPE_OBJ only: the element stride, i.e.
    // sizeof(the element struct). Unused for all other types.
    uint16_t                           elem_size;
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
// The pure walker
// ---------------------------------------------------------------------------

// Walks desc->fields against snap (the snapshot struct's base address),
// driving emit for each present field in table order. No heap, no locks,
// no I/O, no format knowledge -- purely a descriptor interpreter. Does NOT
// wrap the walked fields in a begin_obj/end_obj pair itself; a backend's
// one-shot entry point (e.g. bb_serialize_json()) does that around the
// call if the wire format wants a root container.
void bb_serialize_walk(const bb_serialize_desc_t *desc, const void *snap,
                        const bb_serialize_emit_t *emit);

// Top-level field lookup by key (not recursive into BB_TYPE_OBJ children)
// -- e.g. for a future direct-read display path. Returns NULL if desc,
// key, or a matching field is absent.
const bb_serialize_field_t *bb_serialize_desc_find(const bb_serialize_desc_t *desc,
                                                    const char *key);

#ifdef __cplusplus
}
#endif
