#pragma once

#include <stdint.h>
#include <stdbool.h>

// Pure idle-accumulation helper used by bb_sse_writer.
// Advances the idle accumulator by step_ms; sets *should_ping and resets
// the accumulator to 0 when it reaches or exceeds heartbeat_ms.
// Returns the new accumulator value.
uint32_t bb_sse_idle_advance(uint32_t accumulated, uint32_t step_ms,
                             uint32_t heartbeat_ms, bool *should_ping);
