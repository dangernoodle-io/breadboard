#pragma once

#include "bb_log.h"

/**
 * Internal backend implementation. Called by the portable bb_log_level_set wrapper
 * after registry update. Platform-specific: esp_log_level_set on ESP-IDF, no-op elsewhere.
 */
void _bb_log_level_set_backend(const char *tag, bb_log_level_t level);

/**
 * Host-only: reset registry for testing. Compiled out on ESP-IDF.
 */
#ifndef ESP_PLATFORM
void _bb_log_registry_reset(void);
#endif

// ---------------------------------------------------------------------------
// bb_log_flush() wait-loop decision core (components/bb_log/src/
// bb_log_flush_wait.c) -- pure, portable, host-testable. Compiled on every
// platform; only the ESP-IDF glue in platform/espidf/bb_log/bb_log.c that
// drives FreeRTOS calls (xSemaphoreTake/xQueueSend/tick arithmetic) around
// these is platform-specific. Extracted so the sequence-number/stale-give
// protocol itself has real test coverage instead of living entirely inside
// untestable FreeRTOS glue (a previous version of this code had a real bug
// here -- a stale semaphore give from a timed-out caller's abandoned marker
// could be consumed by the NEXT caller's take, letting it return BB_OK
// without its own marker ever being reached).
// ---------------------------------------------------------------------------

typedef enum {
    BB_LOG_FLUSH_WAIT_DONE,     // my marker's give arrived -- flush complete
    BB_LOG_FLUSH_WAIT_RETRY,    // a stale/earlier give was consumed -- keep waiting on the remaining budget
    BB_LOG_FLUSH_WAIT_TIMEOUT,  // the semaphore take itself timed out -- deadline expired
} bb_log_flush_wait_step_t;

/**
 * Wraparound-safe "has the marker with sequence number target_seq already
 * been completed" check, given the writer's most recently completed
 * sequence number. Uses signed-difference comparison (the same idiom TCP
 * sequence numbers use): (int32_t)(completed_seq - target_seq) >= 0. Correct
 * across a uint32_t wraparound as long as fewer than 2^31 markers are ever
 * outstanding between the two values being compared -- unreachable in
 * practice (bb_log_flush() serializes callers one at a time via a mutex, so
 * at most a handful of stale markers can ever be queued ahead of a live
 * one).
 */
bool bb_log_flush_seq_reached(uint32_t completed_seq, uint32_t target_seq);

/**
 * Pure decision for one iteration of bb_log_flush()'s wait loop, given the
 * outcome of the semaphore take just attempted:
 *  - take_succeeded == false: the take itself timed out -> TIMEOUT.
 *  - take_succeeded == true and completed_seq has reached target_seq (see
 *    bb_log_flush_seq_reached) -> DONE, my own marker was drained.
 *  - take_succeeded == true but completed_seq has NOT reached target_seq ->
 *    RETRY: the give just consumed belongs to an earlier caller's
 *    abandoned/timed-out marker, still sitting in the queue ahead of mine --
 *    keep waiting on the remaining budget rather than falsely report BB_OK.
 * No I/O, no clock -- host-testable in isolation from the real FreeRTOS
 * semaphore/queue.
 */
bb_log_flush_wait_step_t bb_log_flush_wait_decide(bool take_succeeded,
                                                   uint32_t completed_seq,
                                                   uint32_t target_seq);

/**
 * Remaining wait budget in ms, clamped to 0 (never negative/wrapped) once
 * elapsed_ms has consumed the whole budget_ms. Used to keep
 * bb_log_flush()'s retry loop bounded by a SINGLE overall deadline
 * (LOG_FLUSH_TIMEOUT_MS) rather than restarting the full timeout on every
 * RETRY iteration. Pure arithmetic -- no clock access, host-testable.
 */
uint32_t bb_log_flush_remaining_ms(uint32_t budget_ms, uint32_t elapsed_ms);

// ---------------------------------------------------------------------------
// bb_log_line_parse() -- pure, portable, host-testable parser for the
// console-format log line s_log_vprintf produces (components/bb_log/src/
// bb_log_line_parse.c). Relocated here from bb_log_event (B1-831 PR-1) so
// bb_log itself can reuse it in a later PR without bb_log depending back on
// bb_log_event -- see the REQUIRES comment at the top of this component's
// CMakeLists.txt (KB #708/#704) for why that dependency direction is fixed.
// ---------------------------------------------------------------------------

/**
 * Pure log-line parser. Compiled on both host and ESP-IDF (no platform deps).
 * Parses ESP-IDF console format: "<L> (<ts>) <tag>: <msg>"
 * Strips leading ANSI CSI escape sequences and trailing CR/LF.
 * On parse failure: level_out='?', tag_out="", msg_out=<trimmed line>.
 * msg is bounded to 160 bytes before copying into msg_out.
 */
void bb_log_line_parse(const char *line, size_t len,
                       char *level_out,
                       char *tag_out, size_t tag_cap,
                       char *msg_out, size_t msg_cap);

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Install the event-forwarder queue in s_log_vprintf.
// Called once by bb_log_event.c during its init. Pass NULL to disable.
// Non-blocking enqueue (drop-on-full) so the IDF log mutex is never held long.
void bb_log_event_set_queue(QueueHandle_t q);

// Retrieve dropped-enqueue counter for the event forwarder queue.
uint32_t bb_log_event_dropped(void);
#endif
