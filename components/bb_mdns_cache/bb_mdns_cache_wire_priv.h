#pragma once

// bb_mdns_cache_wire — private wire descriptor (SSOT) for a single
// bb_mdns_cache peer entry (B1-1115 PR-3).
//
// SHAPE CHANGE from the legacy bb_json emitter (platform/espidf/
// bb_mdns_cache/bb_mdns_cache.c's entry_serialize()) -- this is a
// deliberate, DOCUMENTED divergence, not a bug. Every consumer of this
// component is frozen pending a rewrite (TaipanMiner frozen, taipan-brood
// being rewritten), so there is no back-compat obligation and byte-parity
// with the legacy emitter was dropped as the acceptance bar (KB 1384:
// byte-identity comes from render determinism, not legacy wire-compat).
//
//   OLD (entry_serialize, TXT-less):        {"hostname":"...","ip4":"...","port":1234}
//   OLD (entry_serialize, TXT-configured):  {"hostname":"...","ip4":"...","port":1234,"board":"...",...}
//     -- TXT fields spliced flat into the top-level object, one wire key per
//        configured bb_mdns_txt_field_t.txt_key.
//
//   NEW (this descriptor, always):          {"hostname":"...","ip4":"...","port":1234,"txt":[]}
//   NEW (this descriptor, TXT-configured):  {"hostname":"...","ip4":"...","port":1234,
//                                             "txt":[{"key":"board","value":"..."}, ...]}
//
// Why: entry_serialize()'s TXT extension is fundamentally dynamic --
// bb_mdns_cache_config_t.txt_fields is an arbitrary `bb_mdns_txt_field_t[]`
// table supplied at RUNTIME by whichever consumer calls
// bb_mdns_cache_start(), naming keys bb_mdns_cache itself has no compile-time
// knowledge of. A static bb_serialize_field_t table cannot name a wire key
// it doesn't know at compile time, so splicing TXT fields flat into the
// object (matching the old shape) is not representable. Instead, "txt" is a
// bounded BB_TYPE_ARR of {"key","value"} objects -- both STR fields, so the
// array element shape is fully static regardless of which keys a given
// consumer configures. This is the natural bb_serialize shape for a
// dynamically-keyed set, not a distortion to preserve legacy compat.
// `hostname`/`ip4`/`port` are unchanged from entry_serialize()'s own field
// names/order/types. No field is dropped; `txt` is additive whenever the
// legacy shape spliced values into the object at all.
//
// Portable: no ESP-IDF/FreeRTOS types, compiles on host + ESP-IDF (mirrors
// components/bb_wifi_http/bb_wifi_http_wire_priv.h's pattern). ADDITIVE
// ONLY: no bb_data_bind() call exists anywhere in this PR, matching the
// staging bb_diag_boot_wire.h is currently in (see
// test/test_host/test_wire_desc_producers.c's file header) -- entry_serialize
// and the bb_cache .serialize registration are untouched; this is a second,
// parallel descriptor path a later PR can migrate bb_cache's own
// serialization contract onto.
//
// Included by:
//   - components/bb_mdns_cache/bb_mdns_cache_wire.c (this descriptor's SSOT)
//   - test/test_host/test_bb_mdns_cache_wire.c (exact-JSON shape proof)

#include "bb_serialize.h"
#include "bb_mdns_cache.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bound on the "txt" array's element count -- MUST stay numerically
// identical to BB_MDNS_CACHE_REQUERY_TXT_MAX (platform/espidf/bb_mdns_cache/
// bb_mdns_cache.c's existing cap on how many TXT descriptor fields the
// re-query path walks); that file #includes this header and
// _Static_assert()s the two in lockstep, since the ESP-IDF-gated
// bb_mdns_cache.c and this portable header can't share a single #define.
#define BB_MDNS_CACHE_WIRE_TXT_MAX 8

// One "txt" array element -- key/value are both wire-owned bounded buffers,
// independent of whatever key length / dest_len a given consumer's
// bb_mdns_txt_field_t table declares (bb_mdns_cache_entry_wire_fill() below
// truncates safely via bb_strlcpy -- bounded to min(field->dest_len,
// sizeof(value)), never just sizeof(value), so a value read is never
// scanned past the field's own declared bound even if the source buffer's
// NUL-termination-within-dest_len guarantee were ever violated).
typedef struct {
    char key[24];
    char value[32];
} bb_mdns_cache_txt_wire_t;

// Row descriptor (2 fields) -- shared by the fill and any future consumer
// needing direct access.
extern const bb_serialize_field_t bb_mdns_cache_txt_wire_fields[2];

// One mDNS peer entry. `port` is widened to int64_t -- bb_serialize_walk()'s
// BB_TYPE_I64 case always memcpy()s a fixed 8 bytes at the descriptor offset
// (see bb_serialize_walk.c); pointing a BB_TYPE_I64 field at a narrower
// uint16_t would read past it. `txt_items`/`txt` is the storage/carrier
// split used throughout this codebase (e.g.
// bb_wifi_http_scan_wire_t.aps_items/aps) -- `txt` is always present
// (possibly count == 0, i.e. "txt":[]) rather than omitted, since a
// consumer's TXT-descriptor configuration is fill-time information the
// descriptor itself cannot see.
typedef struct {
    char                      hostname[BB_MDNS_CACHE_HOSTNAME_MAX];
    char                      ip4[BB_MDNS_CACHE_IP4_MAX];
    int64_t                   port;
    bb_mdns_cache_txt_wire_t  txt_items[BB_MDNS_CACHE_WIRE_TXT_MAX];
    bb_serialize_arr_t        txt;
} bb_mdns_cache_entry_wire_t;

_Static_assert(sizeof(((bb_mdns_cache_entry_wire_t *)0)->port) == 8,
               "bb_mdns_cache_entry_wire_t.port must be exactly 8 bytes for BB_TYPE_I64");

extern const bb_serialize_desc_t bb_mdns_cache_entry_wire_desc;

// Fills `dst` from `entry` plus an OPTIONAL TXT descriptor -- pure, no
// locks/clock/I/O (this stays a pure fill deliberately -- it never logs;
// see `out_dropped` below for how a caller surfaces truncation instead).
// Zero-inits `dst` first, then writes every field unconditionally. `dst`
// and `entry` are assumed non-NULL by the caller (same contract as
// bb_wifi_http_info_wire_fill()).
//
// `entry`/`entry_size`/`txt_fields`/`txt_count` mirror
// bb_mdns_cache_txt_serialize()'s own parameters exactly (including its
// untyped `const void *entry` -- the identity fields are read at the
// leading bb_mdns_cache_entry_t layout, but the underlying buffer may be a
// larger CONSUMER-defined struct per bb_mdns_cache_config_t.entry_size's
// contract, so `entry` is never typed as bb_mdns_cache_entry_t* here) --
// this is the SAME descriptor-walk the legacy serializer drives, just
// emitting into `dst->txt_items`/`dst->txt` instead of a bb_json_t.
// txt_fields == NULL or txt_count == 0 leaves `dst->txt` an empty
// (count == 0) array -- the TXT-less case. A field whose [dest_offset,
// dest_offset+dest_len) range would exceed entry_size, or whose txt_key is
// NULL, is skipped (not read), same bounds contract as
// bb_mdns_cache_txt_serialize(). At most BB_MDNS_CACHE_WIRE_TXT_MAX fields
// are captured.
//
// `out_dropped` (nullable) -- counts ONLY entries skipped because the
// BB_MDNS_CACHE_WIRE_TXT_MAX cap was already full when their turn in
// `txt_fields` came up ("dropped because there was no room left"), never
// entries skipped for other reasons (NULL txt_key, or a
// [dest_offset, dest_offset+dest_len) range exceeding entry_size) -- those
// are simply not-captured, not cap overflow, and never bump this count. This
// preserves the pre-restructure contract exactly (same numeric result as
// the old `txt_count > BB_MDNS_CACHE_WIRE_TXT_MAX ? txt_count -
// BB_MDNS_CACHE_WIRE_TXT_MAX : 0` computation), now derived by counting in
// the walk loop instead of a single post-hoc subtraction. Always 0 on the
// txt_fields == NULL / txt_count == 0 early-return path (nothing was
// eligible to walk, so nothing was dropped for lack of room -- this also
// means a NULL txt_fields with a nonzero txt_count no longer reports a
// bogus nonzero drop, since the loop that increments it never runs). This
// is a SIGNAL, not a log -- the fill itself stays pure/silent by design. A
// caller wiring this to a real producer owns surfacing it loudly, mirroring
// platform/espidf/bb_mdns_cache/bb_mdns_cache.c's own
// `bb_log_w(TAG, "requery: %s has %u TXT records, truncating to %d", ...)`
// precedent for the identical BB_MDNS_CACHE_REQUERY_TXT_MAX overflow case --
// do not let this drop stay silent once something actually calls this fill.
void bb_mdns_cache_entry_wire_fill(bb_mdns_cache_entry_wire_t *dst,
                                    const void *entry, size_t entry_size,
                                    const bb_mdns_txt_field_t *txt_fields,
                                    size_t txt_count, size_t *out_dropped);

#ifdef __cplusplus
}
#endif
