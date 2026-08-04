#pragma once

// floor_task_stack — pure decision for floor's "possibly truncated" tasks-
// report warning (firmware review MEDIUM finding on B1-1418 PR-2).
// examples/floor is a hand-wired composition root, not a component (see
// CLAUDE.md's composition-only model) -- floor_app.c's task_stack_log_tick
// calls this function directly, and
// test/test_host/test_floor_task_stack.c #includes floor_task_stack.c
// directly (examples are not part of the native scaffold's component
// graph, so this is the only way to reach it from a host test) -- one code
// path, no mirror.
//
// WHY THIS EXISTS: floor's "tasks" bb_data row producer
// (bb_serialize_console_tasks_gather(), components/bb_serialize_console)
// gathers into a caller-owned, fixed-capacity scratch array
// (floor_app.c's FLOOR_TASK_STACK_ROWS_MAX, deliberately NOT tied to
// bb_task.h's BB_TASK_BASE_MAX_CAP -- see floor_app.c's own doc comment on
// that constant for the REQUIRES-decoupling rationale). BB_TASK_BASE_MAX
// (components/bb_task/Kconfig) is a live `range 1 64` Kconfig, default 24 --
// a board that legally raises it above floor's fixed row-scratch capacity
// grows the real task registry, but bb_serialize_console_tasks_gather()'s
// own gather_cb (bb_serialize_console_tasks.c) truncates SILENTLY once the
// row count reaches the caller's cap: no error, no log, nothing else in the
// chain signals it. Tasks simply stop appearing in the report.
//
// bb_serialize_console_tasks_gather() returns an ALREADY-CLAMPED count --
// its gather_cb stops incrementing once count >= cap, so the return value
// can never exceed the caller's `max_rows`; the guard below therefore tests
// `emitted == max_rows` (not >=) -- an emitted > max_rows arm would be dead
// by construction at this fn's one real call site, so the predicate is
// narrowed to exactly what that call site can produce, not widened for a
// case that can't occur. == max_rows is still an IMPRECISE but SOUND
// "possibly truncated" signal: it also fires on the legitimate case of a
// registry that has EXACTLY max_rows entries with nothing actually
// dropped -- there is no way to tell the two apart from this return value
// alone (bb_task_base_foreach() reports no true total). That imprecision is
// inherent and unchanged by the == narrowing; hence "maybe"/"possibly" in
// every name/message here, never "truncated" unqualified.
#include <stdbool.h>
#include <stddef.h>

// Decides whether task_stack_log_tick() should log its one-time "possibly
// truncated" warning this tick. `emitted` is the row count
// bb_data_render_rows() actually emitted this tick (mirrors
// bb_serialize_console_tasks_gather()'s own already-clamped return, see
// above); `max_rows` is the caller's row-scratch capacity
// (FLOOR_TASK_STACK_ROWS_MAX); `already_warned` is the caller-owned latch
// (floor_app.c's s_tasks_truncation_warned) -- this fn does not mutate
// state itself, it only decides; the caller flips its own latch when this
// returns true, so the warning fires at most once per boot, not every tick
// a full-or-overflowing registry persists.
//
// Returns false when `max_rows` is 0 (no rows are ever possible, so
// `emitted == max_rows` would trivially match 0 == 0 with nothing to warn
// about) or when `already_warned` is true.
bool floor_task_stack_should_warn_truncated(size_t emitted, size_t max_rows, bool already_warned);
