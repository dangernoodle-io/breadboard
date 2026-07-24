#pragma once

// bb_diag_sockets_get_wire — private wire descriptor (SSOT) for the GET
// /api/diag/sockets response (B1-1190 diag conversion, B1-1054 stream).
// Migration of sockets_get_handler's hand-streamed bb_http_resp_json_obj_*
// emitter (platform/espidf/bb_diag_http/bb_diag_http_routes.c) to a
// bb_serialize descriptor rendered via bb_http_serialize_stream(). Locked
// design (do NOT reintroduce a fork):
//
// Shape (mirrors s_sockets_get_responses[]'s hand-authored OpenAPI literal,
// same file):
//   {"lwip_max_sockets","in_use",
//    "by_state":{"CLOSED","LISTEN","SYN_SENT","SYN_RCVD","ESTABLISHED",
//                "FIN_WAIT_1","FIN_WAIT_2","CLOSE_WAIT","CLOSING",
//                "LAST_ACK","TIME_WAIT"},
//    "pcbs":[{"local_port","remote_ip","remote_port","state"},...]}
//
// All top-level fields, all 11 by_state fields, and every per-PCB field are
// unconditionally present (no `.present` predicate anywhere) -- the
// pre-migration handler already emitted the full fixed by_state object
// (zeros included) and lwip_max_sockets/in_use every call, so this is
// byte-identical runtime output, just a tighter schema: by_state was
// previously published as `additionalProperties:{"type":"integer"}`
// (imprecise); this migration ALSO updates the hand-authored
// s_sockets_get_responses[] literal to list the same 11 named required
// integer fields, so the published schema now matches both runtime reality
// and this table exactly (see bb_diag_http_routes.c's sockets_get_handler
// banner + test_bb_diag_sockets_get_wire_meta_golden.c for the byte-fidelity
// proof). The runtime JSON itself is unchanged.
//
// Heap-allocation deviation (READ BEFORE "fixing" this into a stack local):
// unlike bb_diag_panic_get_wire_t/bb_ota_validator_partitions_wire_t (small,
// bounded stack snapshots), this wire struct carries
// `pcbs_items[BB_DIAG_SOCKETS_ROW_CAP]` sized to track CONFIG_LWIP_MAX_SOCKETS
// (see below) -- a fleet may raise that Kconfig well past the historical
// default (LWIP socket-budget epic B1-745), which would grow this struct
// past a safe stack-frame size on a no-PSRAM-class ESP32 task stack. The
// live handler (sockets_get_handler) therefore heap-allocates the WHOLE
// snapshot via bb_malloc_prefer_spiram(sizeof(*dst)) rather than declaring
// it as a stack local -- this is a DELIBERATE exception to this component's
// otherwise-stack-snapshot convention, not an oversight. Do not revert it to
// a stack local without re-deriving the worst-case CONFIG_LWIP_MAX_SOCKETS
// stack cost first.
//
// bb_diag_sockets_get_wire_fill()/bb_diag_sockets_get_wire_copy_rows() are
// pure populate helpers -- no ESP-IDF/LWIP symbols -- that take the
// already-gathered raw values (the ESP-IDF/LWIP-only PCB walk stays in
// sockets_get_handler, under LOCK_TCPIP_CORE()) and produce a
// host-testable snapshot. Portable: compiles on host + ESP-IDF.
//
// Included by:
//   - platform/espidf/bb_diag_http/bb_diag_http_routes.c (the live handler)
//   - test/test_host/test_bb_diag_sockets_get_wire.c (expected-JSON fixtures)
//   - test/test_host/test_bb_diag_sockets_get_wire_meta_golden.c (meta golden)

#include "bb_serialize.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Capacity (Kconfig bridge) -- tracks CONFIG_LWIP_MAX_SOCKETS, NOT a hand
// constant. A stale hand cap would silently truncate "pcbs" below the live
// PCB pool the moment CONFIG_LWIP_MAX_SOCKETS is raised (B1-745 socket-budget
// epic) -- this bridge makes that impossible by construction.
// ---------------------------------------------------------------------------
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#ifdef CONFIG_LWIP_MAX_SOCKETS
#define BB_DIAG_SOCKETS_ROW_CAP CONFIG_LWIP_MAX_SOCKETS
#endif
#ifndef BB_DIAG_SOCKETS_ROW_CAP
// Host-build default (CONFIG_LWIP_MAX_SOCKETS does not exist off-device).
#define BB_DIAG_SOCKETS_ROW_CAP 16
#endif

// Number of distinct LWIP TCP states this route reports -- CLOSED..TIME_WAIT
// (enum tcp_state's own ordinal range), mirrors the pre-migration handler's
// S_TCP_STATE_COUNT/_Static_assert.
#define BB_DIAG_SOCKETS_STATE_COUNT 11

// Portable (non-LWIP) TCP state name table, indexed by enum tcp_state's own
// ordinal value (0=CLOSED .. 10=TIME_WAIT) -- moved out of
// bb_diag_http_routes.c so both the handler (index -> name for its own
// log line + row src state_idx) and bb_diag_sockets_get_wire_copy_rows()
// below (index -> borrowed bb_serialize_str_n_t) share one SSOT table.
extern const char *const bb_diag_sockets_tcp_state_names[BB_DIAG_SOCKETS_STATE_COUNT];

// ---------------------------------------------------------------------------
// Row source (src-only staging, never wire-emitted directly)
// ---------------------------------------------------------------------------

// One live PCB snapshot row, gathered by sockets_get_handler under
// LOCK_TCPIP_CORE(). `state_idx` is the raw `(uint32_t)p->state` ordinal --
// bb_diag_sockets_get_wire_copy_rows() below resolves it against
// bb_diag_sockets_tcp_state_names[], falling back to "UNKNOWN" for an
// out-of-range value (defensive, mirrors the pre-migration handler's own
// `state_idx < S_TCP_STATE_COUNT ? name : "UNKNOWN"` guard).
typedef struct {
    uint16_t local_port;
    uint16_t remote_port;
    char     remote_ip[40];
    uint32_t state_idx;
} bb_diag_sockets_pcb_src_t;

// ---------------------------------------------------------------------------
// Wire snapshot
// ---------------------------------------------------------------------------

// One row in the "pcbs" array. `remote_ip` is COPIED into a fixed buffer
// (same 40-byte bound as the pre-migration pcb_snap_t); `state` is a
// BORROWED bb_serialize_str_n_t pointing at
// bb_diag_sockets_tcp_state_names[]'s static strings (or the "UNKNOWN"
// literal) -- same convention as bb_ota_validator_partition_wire_t's `state`
// field.
typedef struct {
    int64_t              local_port;
    char                 remote_ip[40];
    int64_t              remote_port;
    bb_serialize_str_n_t state;
} bb_diag_sockets_pcb_wire_t;

// Row descriptor (4 fields) -- shared by the production handler and the host
// tests.
extern const bb_serialize_field_t bb_diag_sockets_pcb_wire_fields[4];
// SSOT field count, computed from the array above -- mirrors
// bb_ota_validator_partition_wire_n_fields's pattern. Callers pass this,
// never a hand-typed literal, so the count can never desync from the array.
extern const uint16_t             bb_diag_sockets_pcb_wire_n_fields;

// "by_state" nested object -- 11 individually-named int64 fields, keys taken
// verbatim from bb_diag_sockets_tcp_state_names[] (CLOSED..TIME_WAIT),
// unconditional (no `.present`) -- mirrors bb_diag_boot_wire_t's "panic"/
// "reboot_reason" nested-BB_TYPE_OBJ precedent.
typedef struct {
    int64_t closed;
    int64_t listen;
    int64_t syn_sent;
    int64_t syn_rcvd;
    int64_t established;
    int64_t fin_wait_1;
    int64_t fin_wait_2;
    int64_t close_wait;
    int64_t closing;
    int64_t last_ack;
    int64_t time_wait;
} bb_diag_sockets_by_state_wire_t;

// Top-level object snapshot. `pcbs_items` is the backing row storage,
// `pcbs` is the bb_serialize_arr_t carrier the descriptor's "pcbs"
// BB_TYPE_ARR field points at -- same storage/carrier split as
// bb_ota_validator_partitions_wire_t's partitions_items/partitions.
//
// SIZE NOTE: `pcbs_items[BB_DIAG_SOCKETS_ROW_CAP]` can be large when
// CONFIG_LWIP_MAX_SOCKETS is raised -- see this file's banner above.
// sockets_get_handler heap-allocates this struct; NEVER declare it as a
// stack local.
typedef struct {
    int64_t                          lwip_max_sockets;
    int64_t                          in_use;
    bb_diag_sockets_by_state_wire_t  by_state;
    bb_diag_sockets_pcb_wire_t       pcbs_items[BB_DIAG_SOCKETS_ROW_CAP];
    bb_serialize_arr_t               pcbs;
} bb_diag_sockets_get_wire_t;

// Top-level object descriptor: renders the shape documented above via
// bb_http_serialize_stream()/bb_serialize_json_render() -- byte-identical to
// the pre-migration hand cJSON emitter.
extern const bb_serialize_desc_t bb_diag_sockets_get_wire_desc;

// bb_serialize_desc_meta_t companion (B1-1059 PR-3a meta-derivation feeder)
// -- co-located JSON Schema docs/validation table for
// bb_diag_sockets_get_wire_desc above, same #if-gated pattern as
// bb_diag_panic_get_wire_priv.h's exemplar. BB_SERIALIZE_META_HOST is a
// host-only define (set by the PlatformIO native env; see platformio.ini) --
// NEVER set by the ESP-IDF/device build, so this declaration (and its
// definition in bb_diag_sockets_get_wire.c) compiles to nothing on-device.
#include "bb_serialize_meta.h"
#if defined(BB_SERIALIZE_META_SHIP)

extern const bb_serialize_desc_meta_t bb_diag_sockets_get_wire_meta;
#endif /* BB_SERIALIZE_META_SHIP */

// Pure row-copy helper: copies `n` rows from `src` into `dst` (row-count
// bounded by BB_DIAG_SOCKETS_ROW_CAP by the caller). Copies `remote_ip` into
// the fixed buffer (bb_strlcpy, always NUL-terminated) and wires `state`'s
// bb_serialize_str_n_t to bb_diag_sockets_tcp_state_names[]'s borrowed
// static string (or "UNKNOWN" for an out-of-range state_idx). Host-testable
// without a live LWIP PCB walk -- the sole reason this is factored out of
// bb_diag_sockets_get_wire_fill() below. `n` MUST be <=
// BB_DIAG_SOCKETS_ROW_CAP -- the caller is the only source of that bound,
// this helper does not itself clamp it.
void bb_diag_sockets_get_wire_copy_rows(bb_diag_sockets_pcb_wire_t *dst,
                                         const bb_diag_sockets_pcb_src_t *src,
                                         size_t n);

// Pure populate helper: zero-inits `dst`, widens/copies the already-gathered
// scalar values (lwip_max_sockets, in_use, the 11 by_state counts), and
// copies `n_rows` PCB rows via bb_diag_sockets_get_wire_copy_rows() above.
// No ESP-IDF/LWIP symbols -- host-testable without a live socket walk.
// `by_state_counts` MUST be indexed by enum tcp_state's own ordinal
// (0=CLOSED..10=TIME_WAIT), same convention as `bb_diag_sockets_pcb_src_t`'s
// `state_idx`. `n_rows` MUST be <= BB_DIAG_SOCKETS_ROW_CAP -- the caller
// (sockets_get_handler) is responsible for that bound.
void bb_diag_sockets_get_wire_fill(bb_diag_sockets_get_wire_t *dst,
                                    uint32_t lwip_max_sockets, uint32_t in_use,
                                    const uint32_t by_state_counts[BB_DIAG_SOCKETS_STATE_COUNT],
                                    const bb_diag_sockets_pcb_src_t *rows, size_t n_rows);

#ifdef __cplusplus
}
#endif
