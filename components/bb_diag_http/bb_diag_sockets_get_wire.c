// bb_diag_sockets_get_wire — wire descriptor + fill for GET
// /api/diag/sockets. See bb_diag_sockets_get_wire_priv.h for the full
// shape/heap-allocation-deviation banner this migration produces.

#include "bb_diag_sockets_get_wire_priv.h"

#include "bb_str.h"

#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// State name table (portable -- see bb_diag_sockets_get_wire_priv.h)
// ---------------------------------------------------------------------------

const char *const bb_diag_sockets_tcp_state_names[BB_DIAG_SOCKETS_STATE_COUNT] = {
    "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD",
    "ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2",
    "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT"
};

// ---------------------------------------------------------------------------
// Descriptor
// ---------------------------------------------------------------------------

const bb_serialize_field_t bb_diag_sockets_pcb_wire_fields[4] = {
    { .key = "local_port", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_sockets_pcb_wire_t, local_port) },
    { .key = "remote_ip", .type = BB_TYPE_STR,
      .offset = offsetof(bb_diag_sockets_pcb_wire_t, remote_ip),
      .max_len = sizeof(((bb_diag_sockets_pcb_wire_t *)0)->remote_ip) },
    { .key = "remote_port", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_sockets_pcb_wire_t, remote_port) },
    { .key = "state", .type = BB_TYPE_STR_N,
      .offset = offsetof(bb_diag_sockets_pcb_wire_t, state) },
};

const uint16_t bb_diag_sockets_pcb_wire_n_fields =
    sizeof(bb_diag_sockets_pcb_wire_fields) / sizeof(bb_diag_sockets_pcb_wire_fields[0]);

// bb_diag_sockets_pcb_wire_n_fields (above) is a `const uint16_t`, not a
// constant expression -- it can't initialize `.n_children` even in this same
// TU. The literal below encodes the documented 4-field row shape above; a
// drift is caught by test_bb_diag_sockets_get_wire_row_field_count_matches
// asserting bb_diag_sockets_pcb_wire_n_fields == 4 against the SAME extern
// the runtime count comes from (mirrors
// bb_ota_validator_partitions_wire.c's precedent).
#define BB_DIAG_SOCKETS_PCB_ROW_N_FIELDS 4

static const bb_serialize_field_t s_diag_sockets_by_state_wire_fields[BB_DIAG_SOCKETS_STATE_COUNT] = {
    { .key = "CLOSED", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_sockets_by_state_wire_t, closed) },
    { .key = "LISTEN", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_sockets_by_state_wire_t, listen) },
    { .key = "SYN_SENT", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_sockets_by_state_wire_t, syn_sent) },
    { .key = "SYN_RCVD", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_sockets_by_state_wire_t, syn_rcvd) },
    { .key = "ESTABLISHED", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_sockets_by_state_wire_t, established) },
    { .key = "FIN_WAIT_1", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_sockets_by_state_wire_t, fin_wait_1) },
    { .key = "FIN_WAIT_2", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_sockets_by_state_wire_t, fin_wait_2) },
    { .key = "CLOSE_WAIT", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_sockets_by_state_wire_t, close_wait) },
    { .key = "CLOSING", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_sockets_by_state_wire_t, closing) },
    { .key = "LAST_ACK", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_sockets_by_state_wire_t, last_ack) },
    { .key = "TIME_WAIT", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_sockets_by_state_wire_t, time_wait) },
};

static const bb_serialize_field_t s_diag_sockets_get_wire_fields[] = {
    { .key = "lwip_max_sockets", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_sockets_get_wire_t, lwip_max_sockets) },
    { .key = "in_use", .type = BB_TYPE_I64,
      .offset = offsetof(bb_diag_sockets_get_wire_t, in_use) },
    { .key = "by_state", .type = BB_TYPE_OBJ,
      .offset = offsetof(bb_diag_sockets_get_wire_t, by_state),
      .children = s_diag_sockets_by_state_wire_fields,
      .n_children = sizeof(s_diag_sockets_by_state_wire_fields) / sizeof(s_diag_sockets_by_state_wire_fields[0]) },
    { .key = "pcbs", .type = BB_TYPE_ARR,
      .offset = offsetof(bb_diag_sockets_get_wire_t, pcbs),
      .elem_type = BB_TYPE_OBJ, .elem_size = sizeof(bb_diag_sockets_pcb_wire_t),
      .max_items = BB_DIAG_SOCKETS_ROW_CAP,
      .children = bb_diag_sockets_pcb_wire_fields,
      .n_children = BB_DIAG_SOCKETS_PCB_ROW_N_FIELDS },
};

const bb_serialize_desc_t bb_diag_sockets_get_wire_desc = {
    .type_name = "bb_diag_sockets_get_wire_t",
    .fields    = s_diag_sockets_get_wire_fields,
    .n_fields  = sizeof(s_diag_sockets_get_wire_fields) / sizeof(s_diag_sockets_get_wire_fields[0]),
    .snap_size = sizeof(bb_diag_sockets_get_wire_t),
};

// ---------------------------------------------------------------------------
// bb_serialize_desc_meta_t (B1-1059 PR-3a meta-derivation feeder) --
// co-located JSON Schema companion to bb_diag_sockets_get_wire_desc above,
// gated behind BB_SERIALIZE_META_HOST (see bb_diag_sockets_get_wire_priv.h's
// banner). "required" mirrors the UPDATED
// platform/espidf/bb_diag_http/bb_diag_http_routes.c's hand-authored
// s_sockets_get_responses[] 200 literal -- every field at every level is
// unconditionally present (no `.present` predicate anywhere in this
// descriptor), so every row here is `.required = true`. See
// test_bb_diag_sockets_get_wire_meta_golden.c for the fidelity proof
// (including the documented ARR-of-OBJ nested-"required" engine gap, same
// precedent as bb_ota_validator_partitions_wire.c).
// ---------------------------------------------------------------------------
#if defined(BB_SERIALIZE_META_SHIP)

static const bb_serialize_field_meta_t s_diag_sockets_pcb_wire_meta_rows[] = {
    { .key = "local_port",  .required = true },
    { .key = "remote_ip",   .required = true },
    { .key = "remote_port", .required = true },
    { .key = "state",       .required = true },
};

static const bb_serialize_field_meta_t s_diag_sockets_by_state_wire_meta_rows[BB_DIAG_SOCKETS_STATE_COUNT] = {
    { .key = "CLOSED",      .required = true },
    { .key = "LISTEN",      .required = true },
    { .key = "SYN_SENT",    .required = true },
    { .key = "SYN_RCVD",    .required = true },
    { .key = "ESTABLISHED", .required = true },
    { .key = "FIN_WAIT_1",  .required = true },
    { .key = "FIN_WAIT_2",  .required = true },
    { .key = "CLOSE_WAIT",  .required = true },
    { .key = "CLOSING",     .required = true },
    { .key = "LAST_ACK",    .required = true },
    { .key = "TIME_WAIT",   .required = true },
};

static const bb_serialize_field_meta_t s_diag_sockets_get_wire_meta_rows[] = {
    { .key = "lwip_max_sockets", .required = true },
    { .key = "in_use",           .required = true },
    { .key = "by_state",         .required = true,
      .children = s_diag_sockets_by_state_wire_meta_rows,
      .n_children = sizeof(s_diag_sockets_by_state_wire_meta_rows) / sizeof(s_diag_sockets_by_state_wire_meta_rows[0]) },
    { .key = "pcbs",             .required = true,
      .children = s_diag_sockets_pcb_wire_meta_rows,
      .n_children = sizeof(s_diag_sockets_pcb_wire_meta_rows) / sizeof(s_diag_sockets_pcb_wire_meta_rows[0]) },
};

const bb_serialize_desc_meta_t bb_diag_sockets_get_wire_meta = {
    .type_name = "bb_diag_sockets_get_wire_t",
    .rows      = s_diag_sockets_get_wire_meta_rows,
    .n_rows    = sizeof(s_diag_sockets_get_wire_meta_rows) / sizeof(s_diag_sockets_get_wire_meta_rows[0]),
};

#endif /* BB_SERIALIZE_META_SHIP */

// ---------------------------------------------------------------------------
// Row copy + fill
// ---------------------------------------------------------------------------

void bb_diag_sockets_get_wire_copy_rows(bb_diag_sockets_pcb_wire_t *dst,
                                         const bb_diag_sockets_pcb_src_t *src,
                                         size_t n)
{
    for (size_t i = 0; i < n; i++) {
        dst[i].local_port  = (int64_t)src[i].local_port;
        dst[i].remote_port = (int64_t)src[i].remote_port;
        bb_strlcpy(dst[i].remote_ip, src[i].remote_ip, sizeof(dst[i].remote_ip));

        const char *state_name = (src[i].state_idx < BB_DIAG_SOCKETS_STATE_COUNT)
                                  ? bb_diag_sockets_tcp_state_names[src[i].state_idx]
                                  : "UNKNOWN";
        dst[i].state = (bb_serialize_str_n_t){ .ptr = state_name, .len = strlen(state_name) };
    }
}

void bb_diag_sockets_get_wire_fill(bb_diag_sockets_get_wire_t *dst,
                                    uint32_t lwip_max_sockets, uint32_t in_use,
                                    const uint32_t by_state_counts[BB_DIAG_SOCKETS_STATE_COUNT],
                                    const bb_diag_sockets_pcb_src_t *rows, size_t n_rows)
{
    memset(dst, 0, sizeof(*dst));

    dst->lwip_max_sockets = (int64_t)lwip_max_sockets;
    dst->in_use           = (int64_t)in_use;

    dst->by_state.closed      = (int64_t)by_state_counts[0];
    dst->by_state.listen      = (int64_t)by_state_counts[1];
    dst->by_state.syn_sent    = (int64_t)by_state_counts[2];
    dst->by_state.syn_rcvd    = (int64_t)by_state_counts[3];
    dst->by_state.established = (int64_t)by_state_counts[4];
    dst->by_state.fin_wait_1  = (int64_t)by_state_counts[5];
    dst->by_state.fin_wait_2  = (int64_t)by_state_counts[6];
    dst->by_state.close_wait  = (int64_t)by_state_counts[7];
    dst->by_state.closing     = (int64_t)by_state_counts[8];
    dst->by_state.last_ack    = (int64_t)by_state_counts[9];
    dst->by_state.time_wait   = (int64_t)by_state_counts[10];

    bb_diag_sockets_get_wire_copy_rows(dst->pcbs_items, rows, n_rows);
    dst->pcbs.items = dst->pcbs_items;
    dst->pcbs.count = n_rows;
}
