#pragma once

// bb_callback_slot — the "single-slot injected callback" idiom, consolidated.
//
// Pattern: a file-static callback slot + a public setter that composition
// wires + a public, null-safe "invoke" that reads its OWN static slot (no
// externally-linked dispatch(cb, ...) symbol — invoke IS the accessor).
// This shape was hand-rolled twice (bb_wifi's ota-validated query; bb_wifi's
// on_disconnect notify) before being pulled out here. Per the consolidation
// rule (CLAUDE.md "Consolidation" bullet / wiki
// Conventions#reuse-or-extract-shared-helpers), the SECOND hand-rolled
// instance of a shared idiom triggers extraction into its central
// component — no "wait for a third", no same-file exception.
//
// Every generated function (setter + invoke) is external and has an
// explicit prototype at its use site (callers declare them in the
// component's own header; this file's dummy test forward-declares its
// dummies) — no undeclared-external symbols, so the shape stays clean even
// under `-Wmissing-prototypes`/`-Wall`.
//
// All macros below expand to a sequence of declarations/definitions that
// already end in `}` — invoke them WITHOUT a trailing semicolon (a trailing
// `;` would be a stray top-level empty declaration).
//
// ---------------------------------------------------------------------------
// BB_CALLBACK_SLOT_RET — value-return-with-default variant, no-arg callback.
//
//   slot        base name for the generated file-static slot
//               (bb_callback_slot_<slot>)
//   cb_type     the callback function-pointer typedef (no-arg)
//   ret_type    invoke's return type (matches cb_type's return type)
//   setter      name of the generated public setter: void setter(cb_type cb)
//   invoke      name of the generated public invoke fn:
//               ret_type invoke(void) { return slot ? slot() : default; }
//   default_val value returned when the slot is NULL
//
// Usage:
//   typedef bool (*my_query_fn)(void);
//   BB_CALLBACK_SLOT_RET(my_query, my_query_fn, bool, my_set_query_cb,
//                        my_query_invoke, true)
//   ...
//   bool v = my_query_invoke();
//
// ---------------------------------------------------------------------------
// BB_CALLBACK_SLOT_VOID0 — void-fire variant, NO-ARG callback.
//
//   slot     base name for the generated file-static slot
//            (bb_callback_slot_<slot>)
//   cb_type  the callback function-pointer typedef (no-arg, void-return)
//   setter   name of the generated public setter: void setter(cb_type cb)
//   invoke   name of the generated public invoke fn:
//            void invoke(void) { if (slot) slot(); }
//
// Usage:
//   typedef void (*my_notify_fn)(void);
//   BB_CALLBACK_SLOT_VOID0(my_notify, my_notify_fn, my_set_notify_cb,
//                          my_notify_invoke)
//   ...
//   my_notify_invoke(); // production call site
//
// ---------------------------------------------------------------------------
// BB_CALLBACK_SLOT_VOID — void-fire variant, callback signature MAY take
// arguments.
//
//   slot       base name for the generated file-static slot
//              (bb_callback_slot_<slot>)
//   cb_type    the callback function-pointer typedef
//   setter     name of the generated public setter: void setter(cb_type cb)
//   invoke     name of the generated public invoke fn:
//              void invoke(<decl_args>) { if (slot) slot(<call_args>); }
//   decl_args  cb's parameter list, PARENTHESIZED, e.g. (const my_evt_t *evt)
//   call_args  the matching argument names to forward to cb, PARENTHESIZED,
//              e.g. (evt)
//
// Usage:
//   typedef void (*my_evt_fn)(const my_evt_t *evt);
//   BB_CALLBACK_SLOT_VOID(my_sink, my_evt_fn, my_set_sink_cb,
//                         my_sink_invoke, (const my_evt_t *evt), (evt))
//   ...
//   my_sink_invoke(evt); // production call site
// ---------------------------------------------------------------------------

// Internal: consumes a parenthesized token group passed as a single macro
// argument (decl_args/call_args above) and splices its contents in place —
// e.g. `BB_CALLBACK_SLOT_EXPAND_ (const my_evt_t *evt)` expands to
// `const my_evt_t *evt`. Not part of the public contract; do not use
// directly. Only BB_CALLBACK_SLOT_VOID (the with-args variant) routes
// through this — BB_CALLBACK_SLOT_VOID0 (no-arg) does not need it.
#define BB_CALLBACK_SLOT_EXPAND_(...) __VA_ARGS__

#define BB_CALLBACK_SLOT_RET(slot, cb_type, ret_type, setter, invoke, default_val) \
    static cb_type bb_callback_slot_##slot = NULL; \
    void setter(cb_type cb) \
    { \
        bb_callback_slot_##slot = cb; \
    } \
    ret_type invoke(void) \
    { \
        cb_type cb = bb_callback_slot_##slot; \
        return cb ? cb() : (default_val); \
    }

#define BB_CALLBACK_SLOT_VOID0(slot, cb_type, setter, invoke) \
    static cb_type bb_callback_slot_##slot = NULL; \
    void setter(cb_type cb) \
    { \
        bb_callback_slot_##slot = cb; \
    } \
    void invoke(void) \
    { \
        cb_type cb = bb_callback_slot_##slot; \
        if (cb) { \
            cb(); \
        } \
    }

#define BB_CALLBACK_SLOT_VOID(slot, cb_type, setter, invoke, decl_args, call_args) \
    static cb_type bb_callback_slot_##slot = NULL; \
    void setter(cb_type cb) \
    { \
        bb_callback_slot_##slot = cb; \
    } \
    void invoke(BB_CALLBACK_SLOT_EXPAND_ decl_args) \
    { \
        cb_type cb = bb_callback_slot_##slot; \
        if (cb) { \
            cb(BB_CALLBACK_SLOT_EXPAND_ call_args); \
        } \
    }
