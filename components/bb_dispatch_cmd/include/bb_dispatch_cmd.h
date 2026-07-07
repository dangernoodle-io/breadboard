// bb_dispatch_cmd — transport-neutral action -> handler command registry.
//
// Sibling of bb_dispatch_api (components/bb_http_server/include/bb_dispatch_api.h):
// that table keys on (method, path) and hands the caller an httpd-flavoured
// bb_http_handler_fn; this one keys on a plain action string and hands the
// caller a bb_json in/out handler with no HTTP concept at all. It ships
// tested-but-unused — no transport wires it up yet. Envelope parsing (the
// wire format that extracts an "action" + "args" from a raw WS/HTTP payload
// and calls bb_dispatch_cmd_call) is a separate later component
// (bb_dispatch_envelope), not part of this registry.
//
// Thread-safety: mirrors bb_dispatch_api — callers register handlers during
// (single-threaded) init before any concurrent bb_dispatch_cmd_call traffic
// starts. bb_dispatch_cmd_call itself does not mutate registry state, only
// looks it up, so concurrent calls are safe once registration is complete.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "bb_core.h"
#include "bb_json.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Dispatch table capacity. Override at build time via Kconfig or -D.
// ---------------------------------------------------------------------------
#ifdef CONFIG_BB_DISPATCH_CMD_CAP
#define BB_DISPATCH_CMD_CAP CONFIG_BB_DISPATCH_CMD_CAP
#else
#define BB_DISPATCH_CMD_CAP 32
#endif

// Handler for a registered action. Reads args, fills result_out, returns
// BB_OK on success or a bb_err_t on failure. result_out is caller-allocated
// (bb_json_obj_new()) and passed in ready to be filled; the handler must not
// free it.
typedef bb_err_t (*bb_dispatch_cmd_handler_t)(bb_json_t args, bb_json_t result_out, void *ctx);

// Optional single authorization checkpoint, consulted before every handler
// invocation. Return true to allow the call, false to reject it. A NULL
// authorizer (the default) allows all calls.
typedef bool (*bb_dispatch_cmd_authorizer_t)(const char *action, bb_json_t args, void *ctx);

// Clear the dispatch table and authorizer (use in tests and re-init).
void bb_dispatch_cmd_test_reset(void);

// Register a handler for an action string.
// action must have static/registry-lifetime storage duration — the registry
// stores the raw pointer, not a copy (safe for the intended
// string-literal-at-init usage).
// Returns:
//   BB_OK                  on success
//   BB_ERR_INVALID_ARG     action or handler is NULL
//   BB_ERR_NO_SPACE        table is full
//   BB_ERR_INVALID_STATE   action already registered (first registration
//                          wins; the duplicate is logged and dropped)
bb_err_t bb_dispatch_cmd_register(const char *action, bb_dispatch_cmd_handler_t handler, void *ctx);

// Install (or clear, with NULL) the single authorization checkpoint used by
// bb_dispatch_cmd_call. A NULL authorizer allows every call.
void bb_dispatch_cmd_set_authorizer(bb_dispatch_cmd_authorizer_t fn, void *ctx);

// Look up action, run the authorizer (if any), and invoke the handler.
// Returns:
//   the handler's own return value on a successful dispatch (result_out
//                          filled) — including BB_ERR_INVALID_STATE if the
//                          handler itself returns it; this is distinct from
//                          (and not to be confused with) an authorizer
//                          rejection below
//   BB_ERR_NOT_FOUND        no handler registered for action
//   BB_ERR_UNAUTHORIZED     the authorizer rejected the call
//   BB_ERR_INVALID_ARG      action or result_out is NULL
bb_err_t bb_dispatch_cmd_call(const char *action, bb_json_t args, bb_json_t result_out);

// Number of actions currently registered. Useful for tests and telemetry.
size_t bb_dispatch_cmd_count(void);

#ifdef __cplusplus
}
#endif
