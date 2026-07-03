#pragma once

#include <stdint.h>
#include <stdbool.h>

// Pure idle-accumulation helper used by bb_sse_writer.
// Advances the idle accumulator by step_ms; sets *should_ping and resets
// the accumulator to 0 when it reaches or exceeds heartbeat_ms.
// Returns the new accumulator value.
uint32_t bb_sse_idle_advance(uint32_t accumulated, uint32_t step_ms,
                             uint32_t heartbeat_ms, bool *should_ping);

// Pure cadence helper used by bb_sse_writer to decouple peer-abort detection
// from a (potentially long) heartbeat/wait interval. Returns the wait slice
// to use for the next wait_fn call: the smaller of remaining_ms and
// abort_poll_ms, so the caller can re-check for peer abort between slices
// without changing the total time spent waiting (the sum of slices returned
// across calls, as remaining_ms is decremented by the caller, equals the
// original wait_timeout_ms). Returns 0 when remaining_ms is 0 (single-shot
// call semantics preserved for a caller-supplied timeout of 0).
uint32_t bb_sse_abort_poll_slice_ms(uint32_t remaining_ms, uint32_t abort_poll_ms);
