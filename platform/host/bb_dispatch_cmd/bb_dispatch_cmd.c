#include "bb_dispatch_cmd.h"
#include "bb_log.h"

#include <stddef.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "dispatch_cmd";

/* ---------------------------------------------------------------------------
 * Internal action entry
 * ---------------------------------------------------------------------------*/
typedef struct {
    const char                *action;
    bb_dispatch_cmd_handler_t  handler;
    void                      *ctx;
} bb_dispatch_cmd_entry_t;

/* ---------------------------------------------------------------------------
 * File-scope state — no heap, no ESP, s_ prefix per house rules
 * ---------------------------------------------------------------------------*/
static bb_dispatch_cmd_entry_t      s_dispatch[BB_DISPATCH_CMD_CAP];
static size_t                       s_count;
static bool                         s_warned;
static bb_dispatch_cmd_authorizer_t s_authorizer;
static void                        *s_authorizer_ctx;

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------*/

void bb_dispatch_cmd_test_reset(void)
{
    memset(s_dispatch, 0, sizeof(s_dispatch));
    s_count          = 0;
    s_warned         = false;
    s_authorizer     = NULL;
    s_authorizer_ctx = NULL;
}

bb_err_t bb_dispatch_cmd_register(const char *action, bb_dispatch_cmd_handler_t handler, void *ctx)
{
    if (action == NULL || handler == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    if (s_count >= BB_DISPATCH_CMD_CAP) {
        return BB_ERR_NO_SPACE;
    }

    /* Duplicate-action safeguard: first registration wins (mirrors
     * bb_dispatch_api's dup policy). */
    size_t action_len = strlen(action);
    for (size_t i = 0; i < s_count; i++) {
        if (strlen(s_dispatch[i].action) != action_len) continue;
        if (memcmp(s_dispatch[i].action, action, action_len) == 0) {
            bb_log_w(TAG, "duplicate action '%s' ignored (first registration wins)", action);
            return BB_ERR_INVALID_STATE;
        }
    }

    s_dispatch[s_count].action  = action;
    s_dispatch[s_count].handler = handler;
    s_dispatch[s_count].ctx     = ctx;
    s_count++;

    /* High-watermark warn: fire once when count crosses CAP-8. */
    if (!s_warned && s_count >= (size_t)(BB_DISPATCH_CMD_CAP - 8)) {
        s_warned = true;
        bb_log_w(TAG, "cmd dispatch table at %u/%u; %d slots remain",
                 (unsigned)s_count, (unsigned)BB_DISPATCH_CMD_CAP,
                 (int)(BB_DISPATCH_CMD_CAP - (int)s_count));
    }

    return BB_OK;
}

void bb_dispatch_cmd_set_authorizer(bb_dispatch_cmd_authorizer_t fn, void *ctx)
{
    s_authorizer     = fn;
    s_authorizer_ctx = ctx;
}

bb_err_t bb_dispatch_cmd_call(const char *action, bb_json_t args, bb_json_t result_out)
{
    if (action == NULL || result_out == NULL) {
        return BB_ERR_INVALID_ARG;
    }

    size_t action_len = strlen(action);

    for (size_t i = 0; i < s_count; i++) {
        /* Every stored entry has a non-NULL action — bb_dispatch_cmd_register
         * rejects NULL up front, unlike bb_dispatch_api's optional path. */
        const char *entry_action = s_dispatch[i].action;
        if (strlen(entry_action) != action_len) {
            continue;
        }
        if (memcmp(entry_action, action, action_len) != 0) {
            continue;
        }

        if (s_authorizer != NULL && !s_authorizer(action, args, s_authorizer_ctx)) {
            bb_log_w(TAG, "action '%s' rejected by authorizer", action);
            return BB_ERR_UNAUTHORIZED;
        }

        return s_dispatch[i].handler(args, result_out, s_dispatch[i].ctx);
    }

    return BB_ERR_NOT_FOUND;
}

size_t bb_dispatch_cmd_count(void)
{
    return s_count;
}
