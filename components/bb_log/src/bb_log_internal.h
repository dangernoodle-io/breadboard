#pragma once

#include "bb_log.h"

/**
 * Internal backend implementation. Called by the portable bb_log_level_set wrapper
 * after registry update. Platform-specific: esp_log_level_set on ESP-IDF, no-op elsewhere.
 */
void _bb_log_level_set_backend(const char *tag, bb_log_level_t level);

/**
 * bb_log_level_t -> the single console-format level letter ESP-IDF's own
 * LOG_FORMAT() macro embeds ('E'/'W'/'I'/'D'/'V'). Pure, portable, host-
 * testable (components/bb_log/src/bb_log_level.c) -- used by bb_log_emit()
 * (B1-1443 PR-1) to build a console-shaped line without going through
 * ESP_LOGx/LOG_FORMAT. BB_LOG_LEVEL_NONE or any out-of-range value maps to
 * '?', the same fallback level char foreign/vendored lines get wrapped
 * with opaque (bb_log_event.c's forwarder, B1-1443 PR-2).
 */
char bb_log_level_console_letter(bb_log_level_t level);

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
// bb_log_telem_route_wide() -- pure, portable, host-testable TELEM
// wide-routing decision core (components/bb_log/src/bb_log_telem_route.c).
// B1-831 PR-2 shipped the knobs + this decision function, no call site.
// B1-831 PR-3 wired a since-removed bb_log_telem_should_route_wide() wrapper
// into s_log_vprintf so a TELEM-tagged line could stay console-only instead
// of also fanning out to the wide sinks. B1-1443 PR-2 deleted that wrapper
// (it depended on bb_log_line_parse(), also deleted): s_log_vprintf now only
// ever sees FOREIGN/vendored ESP_LOGx output (our own bb_log_e/w/i/d/v calls
// bypass it entirely via bb_log_emit(), B1-1443 PR-1), which is wrapped
// opaque with no parsed tag -- so the TELEM gate can never apply to it and
// s_log_vprintf routes every foreign line wide unconditionally. This
// decision core is still called directly by bb_log_emit() for OUR OWN
// lines, which carry a real tag natively (no parse needed): the
// bb_log_event forwarder queue (the live GET /api/events?topic=log
// consumer) and the optional UDP mirror (CONFIG_BB_LOG_UDP_SINK) are gated
// on that route_wide decision. The console writer and the optional bb_diag
// tap are NEVER gated -- every line always reaches them. (The SSE ring this
// comment used to also mention was removed, B1-1409 -- it had zero in-tree
// callers and no live consumer.)
// ---------------------------------------------------------------------------

/**
 * Should this log line also be routed to the wide sinks (ring buffer,
 * bb_log_event forwarder queue, optional UDP mirror), on top of the console
 * writer and the optional bb_diag tap, which every line always reaches
 * regardless of this decision? Framed positively ("should route wide") to
 * avoid a double-negative bug at the call site.
 *
 * - route_events_enabled == true: always true, regardless of tag -- the
 *   TELEM gate is fully open, every tag (TELEM or not) routes wide.
 * - route_events_enabled == false: true for every tag EXCEPT an exact match
 *   against the configured TELEM tag (BB_LOG_TELEM_TAG, bridged from
 *   CONFIG_BB_LOG_TELEM_TAG, default "TELEM") -- a TELEM-tagged line stays
 *   console-only.
 * - tag is NULL or empty: always true (can never equal the non-empty
 *   configured TELEM tag, so it's not a TELEM line).
 * - Comparison is an exact, case-sensitive match -- "TELEMETRY" or any other
 *   prefix/superstring of the configured tag is a DIFFERENT tag and always
 *   routes wide, even when route_events_enabled is false.
 *
 * Pure -- no I/O, no platform calls, no side effects.
 */
bool bb_log_telem_route_wide(bool route_events_enabled, const char *tag);

/**
 * Runtime override for the TELEM wide-routing gate. Boots from
 * CONFIG_BB_LOG_TELEM_ROUTE_EVENTS (default n) -- structurally the same idea
 * as bb_log_level_set() overriding the BB_LOG_LEVELS boot default, but no
 * production consumer is wired to call this yet: today it is reachable only
 * from host tests. Not mutex-protected (plain volatile bool) -- see
 * bb_log_telem_route.c for why that's safe here.
 */
void bb_log_telem_route_set(bool route_events_enabled);

/**
 * Current value of the TELEM wide-routing runtime gate (see
 * bb_log_telem_route_set()).
 */
bool bb_log_telem_route_get(void);

// ---------------------------------------------------------------------------
// bb_log_event forwarder queue item (B1-1443 PR-1) -- the single item type
// for the queue bb_log_event_set_queue() installs (below, ESP-IDF only),
// shared by BOTH producers that enqueue onto it (platform/espidf/bb_log/
// bb_log.c): s_log_vprintf, for FOREIGN/vendored ESP_LOGx output, and
// bb_log_emit(), for OUR bb_log_e/w/i/d/v calls. The one consumer
// (platform/espidf/bb_log_event/bb_log_event.c s_forwarder_task) branches on
// `structured`:
//   - false (foreign): `line`/`len` hold an already-formatted, possibly
//     ANSI-colored console line ("<L> (<ts>) <tag>: <msg>") -- B1-1443 PR-2:
//     the forwarder never parses it. It's wrapped opaque instead (level="?",
//     tag="", msg=the raw line, truncated). `level`/`tag`/`ts_ms` are unused.
//   - true (ours): `level`, `tag`, `ts_ms`, and `line`/`len` (holding just
//     the formatted MESSAGE text, no console decoration) are the real
//     fields bb_log_emit() built at the call site -- no parsing, no
//     forwarder-time timestamp fidelity loss.
// This replaces bb_log_event.c's former private log_event_msg_t, which
// relied on log_writer_msg_t (bb_log.c) and log_event_msg_t sharing an
// identical struct-prefix layout by convention -- now there is one
// authoritative type for both producers instead of a layout-compatibility
// assumption.
//
// Struct itself has no platform dependency (bool/char/uint64_t/size_t only)
// -- kept outside the ESP_PLATFORM guard below so bb_log_emit_build_event_msg()
// (bb_log_emit_build.c) and its host tests can see the real type instead of
// a mirror.
#define BB_LOG_EVENT_MSG_LINE_MAX 192   // mirrors LOG_STREAM_LINE_MAX in bb_log.c/bb_log_event.c
#define BB_LOG_EVENT_MSG_TAG_MAX  48    // mirrors bb_log_event's forwarder tag[] buffer

// Field order: the two 8-byte-aligned members (ts_ms, len) lead, so no
// alignment padding is inserted ahead of either of them -- a `bool level
// tag[48] ts_ms ...` order (bool/char/tag first) leaves 6 bytes of dead
// padding between tag[] and ts_ms on every ABI this compiles for. Trailing
// members (structured/level/tag/line) are all 1-byte-aligned, so they pack
// with no further gaps; the struct's overall size is still rounded up to
// alignof(uint64_t) at the end, but that tail padding is unavoidable either
// way. Net effect of this order vs. the naive one: identical on a host build
// (size_t is 8 bytes there, so len's own padding need is the same as
// ts_ms's), but 8 bytes smaller on-device (ESP32: size_t is 4 bytes, so the
// device build recovers the 6 padding bytes this order removes plus 2 more
// from len's own tighter packing) -- see the _Static_assert below, pinned to
// the (larger) host-verified value.
typedef struct {
    uint64_t ts_ms;                           // structured only
    size_t   len;                             // length of `line`
    bool     structured;
    char     level;                          // structured only
    char     tag[BB_LOG_EVENT_MSG_TAG_MAX];   // structured only
    char     line[BB_LOG_EVENT_MSG_LINE_MAX]; // structured: msg text; foreign: full console line
} bb_log_event_msg_t;

// Pins the struct's heap-per-queue-slot cost so a future field addition
// can't silently blow past what CONFIG_BB_LOG_EVENT_QUEUE_LEN's sizing
// comments (bb_log_event.c) assume. Bound is keyed on sizeof(size_t) rather
// than a single fixed number, because `len` (size_t) is 8 bytes on a 64-bit
// host build but only 4 bytes on-device -- a single loose bound covering
// both would let the struct silently grow up to 8 bytes on the platform
// (ESP32) where the per-slot heap budget is the actual constraint. Both
// values are cross-compiler-verified, not estimated: 264 on a 64-bit host
// build (native gcc) and 256 on real ESP32 (xtensa-esp32-elf-gcc 14.2.0,
// 32-bit size_t) -- the field order above (8-byte-aligned members first)
// is what makes the device build smaller. Widen deliberately if a real
// field is added; do not bump it to silence a regression.
_Static_assert(sizeof(bb_log_event_msg_t) <= (sizeof(size_t) == 8 ? 264 : 256),
               "bb_log_event_msg_t grew past its pinned heap-per-slot budget");

// ---------------------------------------------------------------------------
// bb_log_emit() record-construction core (B1-1443 PR-1, components/bb_log/
// src/bb_log_emit_build.c) -- pure, portable, host-testable. bb_log_emit()
// (platform/espidf/bb_log/bb_log.c) is the only call site; extracted so the
// four-way fan-out's actual record shapes have real test coverage instead of
// living entirely inside untestable ESP-IDF/FreeRTOS glue.
// ---------------------------------------------------------------------------

/**
 * Build the console-shaped log line ("<L> (<ts>) <tag>: <msg>\n") the
 * console writer queue and (when enabled) the UDP mirror both consume --
 * the same shape ESP_LOGx's own LOG_FORMAT() has always produced. `tag`
 * NULL normalizes to "?" (mirrors bb_log_emit's own guard); `msg` NULL is
 * treated as a zero-length message. Bounded to out_cap - 1 bytes (NUL-
 * terminated by snprintf) same as every other bb_log fixed-buffer helper.
 * Returns the number of bytes written (excluding NUL), or 0 if out is NULL,
 * out_cap is 0, or the underlying snprintf failed.
 *
 * `msg_len` invariant: the caller must not pass a length exceeding
 * strlen(msg) -- enforced defensively here via strnlen(msg, msg_len), so a
 * future caller with a stale/oversized msg_len reads at most strlen(msg)
 * bytes rather than walking past the buffer via "%.*s". Today's only call
 * site (bb_log_emit(), platform/espidf/bb_log/bb_log.c) always derives
 * msg_len from bb_log_stream_format()'s (vsnprintf's) own return value, so
 * this is a defense against a future second caller, not a fix for a live bug.
 *
 * No ANSI colour: unlike ESP-IDF's own LOG_FORMAT() (esp_log_format.h),
 * which wraps the level letter in LOG_COLOR_<letter>/LOG_RESET_COLOR under
 * CONFIG_LOG_COLORS, this function never emits colour codes -- BY DESIGN,
 * not an oversight. bb_log_emit() produces a STRUCTURED record (level/tag/
 * ts/msg); this plain-text line is one RENDERING of that record for the
 * console writer/UDP mirror. Colour is a rendering concern that belongs to
 * a console serializer, not to the record producer -- the same precedent
 * B1-1409 set when it removed the raw-console-bytes-with-ANSI SSE ring
 * rather than growing a second producer-side data path for it: if raw
 * ANSI-decorated bytes are ever wanted again, they're served by another
 * serializer over the same record, not by teaching this function to emit
 * escape codes. Known, accepted consequence: under CONFIG_LOG_COLORS=y our
 * bb_log_e/w/i/d/v lines render plain while foreign/vendored ESP_LOGx
 * output (still routed through the original vprintf hook) renders coloured
 * -- a visible but deliberate divergence, not a bug (CONFIG_LOG_COLORS is
 * unset -- IDF default n -- in every sdkconfig this repo ships today, so
 * it is currently latent).
 */
size_t bb_log_emit_build_line(char *out, size_t out_cap, char level_ch, uint64_t ts_ms,
                               const char *tag, const char *msg, size_t msg_len);

/**
 * Populate a bb_log_event_msg_t's structured fields (level/tag/ts_ms/
 * line/len, structured=true) from the level/tag/ts/already-formatted-
 * message bb_log_emit() has at its call site -- the exact record the
 * bb_log_event forwarder queue consumes, no parsing required on the
 * consumer side. `tag` NULL normalizes to "?"; truncates into emsg->tag
 * with bb_strlcpy semantics. `msg` NULL is treated as a zero-length
 * message; otherwise truncates into emsg->line/len at
 * BB_LOG_EVENT_MSG_LINE_MAX - 1 and is NUL-terminated. No-op if emsg is
 * NULL.
 *
 * Same msg_len invariant and defensive strnlen(msg, msg_len) clamp as
 * bb_log_emit_build_line() -- see that function's doc comment above.
 */
void bb_log_emit_build_event_msg(bb_log_event_msg_t *emsg, char level_ch, const char *tag,
                                  uint64_t ts_ms, const char *msg, size_t msg_len);

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
