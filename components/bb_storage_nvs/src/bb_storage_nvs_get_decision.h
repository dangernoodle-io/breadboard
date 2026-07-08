#pragma once
// bb_storage_nvs_get_decision — pure decision logic for nvs_vt_get's four
// read-outcome branches, extracted from platform/espidf/bb_storage_nvs/
// bb_storage_nvs.c so Coveralls sees every branch and the host test suite
// exercises it without NVS. No ESP-IDF/NVS types — host-testable in
// isolation, mirroring sse_pool_reclaim_decision.h / bb_diag_reset_decision.c.
//
// `reserve` accounts for backends whose underlying read call needs headroom
// beyond `required` bytes to succeed even when reading straight into the
// caller's buffer — e.g. NVS's nvs_get_str requires the caller-supplied
// length to cover the string bytes *plus* the NUL terminator, while
// `required`/`out_len` stay NUL-excluded per bb_storage's get() contract.
// Blob callers pass reserve=0 (no such headroom needed); the str caller
// passes reserve=1.
//
// The four outcomes:
//   - cap == 0                                          -> PROBE    (size-probe only)
//   - cap >= required + reserve                         -> FULL     (read straight into caller's buf)
//   - 0 < cap < required+reserve && required+reserve <= max -> BOUNCE   (stage in scratch, copy cap bytes)
//   - 0 < cap < required+reserve && required+reserve > max  -> NO_SPACE (cannot safely stage)
//
// See bb_storage.h's get() contract comment for why NVS (which cannot
// partial-read a blob) bounds truncating reads to scratch_max while other
// backends (e.g. ram) may truncate freely.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BB_STORAGE_NVS_GET_PROBE = 0,  // cap == 0: caller wants only the true length
    BB_STORAGE_NVS_GET_FULL,       // cap >= required: read directly into caller's buf
    BB_STORAGE_NVS_GET_BOUNCE,     // 0 < cap < required <= scratch_max: bounce via scratch
    BB_STORAGE_NVS_GET_NO_SPACE,   // 0 < cap < required, required > scratch_max: cannot service
} bb_storage_nvs_get_outcome_t;

// Pure state -> outcome mapping. *out_len is always set to `required`
// (when non-NULL) regardless of outcome, mirroring bb_storage_get()'s "true
// length regardless of truncation" contract — even the NO_SPACE outcome
// reports the true length so the caller can retry with an adequate buffer.
// `reserve` is extra headroom the underlying read call needs beyond
// `required` bytes (0 for blob reads; 1 for NUL-terminated string reads).
// The caller performs the actual NVS I/O; this function only decides which
// of the four read paths to take.
bb_storage_nvs_get_outcome_t bb_storage_nvs_get_decide(size_t required, size_t cap,
                                                        size_t scratch_max, size_t reserve,
                                                        size_t *out_len);

#ifdef __cplusplus
}
#endif
