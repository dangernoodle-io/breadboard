// floor_task_stack — see floor_task_stack.h for the full contract.

#include "floor_task_stack.h"

bool floor_task_stack_should_warn_truncated(size_t emitted, size_t max_rows, bool already_warned)
{
    if (already_warned || max_rows == 0) {
        return false;
    }
    // == , not >=: bb_serialize_console_tasks_gather()'s real (and only)
    // call site guarantees `emitted` is already clamped to `max_rows` and
    // can never exceed it (see floor_task_stack.h's doc) -- an
    // emitted > max_rows arm would be dead by construction, unreachable
    // from any real caller, existing only to be covered by a synthetic
    // test. Narrowed to the exact predicate the one real call site can
    // actually produce.
    return emitted == max_rows;
}
