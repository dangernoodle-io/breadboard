#include "bb_http_serialize_error.h"
#include "bb_http_serialize_stream.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    char error[BB_HTTP_SERIALIZE_ERROR_MSG_MAX];
} error_wire_t;

static const bb_serialize_field_t s_error_wire_fields[] = {
    { .key = "error", .type = BB_TYPE_STR, .offset = offsetof(error_wire_t, error),
      .max_len = sizeof(((error_wire_t *)0)->error) },
};

static const bb_serialize_desc_t s_error_wire_desc = {
    .type_name = "error_wire_t",
    .fields    = s_error_wire_fields,
    .n_fields  = 1,
    .snap_size = sizeof(error_wire_t),
};

bb_err_t bb_http_serialize_send_error(bb_http_request_t *req, int status, const char *msg)
{
    if (!req) return BB_ERR_INVALID_ARG;

    bb_err_t status_err = bb_http_resp_set_status(req, status);
    if (status_err != BB_OK) return status_err;

    // strnlen-bounded by bb_serialize_walk.c's BB_TYPE_STR read regardless --
    // this copy just gives `msg` its own short-lived, appropriately-sized
    // stack home rather than relying on the caller's storage.
    error_wire_t snap;
    memset(&snap, 0, sizeof(snap));
    if (msg) {
        strncpy(snap.error, msg, sizeof(snap.error) - 1);
    }

    return bb_http_serialize_stream(req, &s_error_wire_desc, &snap);
}
