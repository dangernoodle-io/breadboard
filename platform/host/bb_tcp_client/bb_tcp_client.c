// bb_tcp_client — host backend: no real socket. Hermetic fake matching
// bb_http_client's host-stub philosophy — real network access is not
// implemented on host (host tests must not touch the network). Backed by a
// Kconfig-sized static instance pool (BB_TCP_CLIENT_MAX_INSTANCES); zero
// heap.
#include "bb_tcp_client.h"
#include "bb_tcp_client_priv.h"

#include <string.h>

#ifndef BB_TCP_CLIENT_TEST_BUF_BYTES
#define BB_TCP_CLIENT_TEST_BUF_BYTES 512
#endif

typedef struct {
    bool                   in_use;
    bb_tcp_client_cfg_t    cfg;
    bb_tcp_client_state_t  state;
#ifdef BB_TCP_CLIENT_TESTING
    bool     forced_connect_result_set;
    bb_err_t forced_connect_result;
    bool     forced_io_result_set;
    bb_err_t forced_io_result;
    uint8_t  readable_buf[BB_TCP_CLIENT_TEST_BUF_BYTES];
    size_t   readable_len;
    size_t   readable_pos;
    uint8_t  last_write[BB_TCP_CLIENT_TEST_BUF_BYTES];
    size_t   last_write_len;
    int      write_count;
#endif
} bb_tcp_client_inst_t;

// Single-writer-per-instance invariant: each instance is driven by exactly
// one caller task from init through destroy (no concurrent handle sharing),
// matching bb_udp_client / bb_mqtt_client host stub conventions — no lock
// needed here.
static bb_tcp_client_inst_t s_pool[BB_TCP_CLIENT_MAX_INSTANCES];

static bb_tcp_client_inst_t *inst_from_handle(bb_tcp_client_t h)
{
    bb_tcp_client_inst_t *inst = (bb_tcp_client_inst_t *)h;
    if (!inst || !inst->in_use) return NULL;
    return inst;
}

bb_err_t bb_tcp_client_init(const bb_tcp_client_cfg_t *cfg_or_null, bb_tcp_client_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    if (cfg_or_null &&
        strnlen(cfg_or_null->host, sizeof(cfg_or_null->host)) >= sizeof(cfg_or_null->host)) {
        return BB_ERR_INVALID_ARG;
    }

    int idx = -1;
    for (int i = 0; i < BB_TCP_CLIENT_MAX_INSTANCES; i++) {
        if (!s_pool[i].in_use) { idx = i; break; }
    }
    if (idx < 0) return BB_ERR_NO_SPACE;

    bb_tcp_client_inst_t *inst = &s_pool[idx];
    memset(inst, 0, sizeof(*inst));

    if (cfg_or_null) {
        inst->cfg = *cfg_or_null;
        bb_tcp_client_priv_save_to_nvs(cfg_or_null);
    } else {
        bb_tcp_client_priv_load_from_nvs(&inst->cfg);
    }

    inst->in_use = true;
    inst->state = BB_TCP_CLIENT_DISCONNECTED;
    *out = (bb_tcp_client_t)inst;
    return BB_OK;
}

bb_err_t bb_tcp_client_connect(bb_tcp_client_t h)
{
    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst) return BB_ERR_INVALID_ARG;
    if (inst->state == BB_TCP_CLIENT_CONNECTED) return BB_OK;  // idempotent

#ifdef BB_TCP_CLIENT_TESTING
    if (inst->forced_connect_result_set) {
        bb_err_t rc = inst->forced_connect_result;
        inst->forced_connect_result_set = false;
        if (rc == BB_OK) {
            inst->state = BB_TCP_CLIENT_CONNECTED;
        }
        bb_tcp_client_priv_health_report(rc == BB_OK);
        return rc;
    }
#endif

    // No real socket on host: connect always "succeeds".
    inst->state = BB_TCP_CLIENT_CONNECTED;
    bb_tcp_client_priv_health_report(true);
    return BB_OK;
}

bb_err_t bb_tcp_client_read(bb_tcp_client_t h, uint8_t *buf, size_t len, size_t *out_len)
{
    if (!buf || !out_len) return BB_ERR_INVALID_ARG;
    *out_len = 0;

    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst) return BB_ERR_INVALID_ARG;
    if (inst->state != BB_TCP_CLIENT_CONNECTED) return BB_ERR_INVALID_STATE;

#ifdef BB_TCP_CLIENT_TESTING
    if (inst->forced_io_result_set) {
        bb_err_t rc = inst->forced_io_result;
        inst->forced_io_result_set = false;
        if (rc != BB_OK && rc != BB_ERR_TIMEOUT) {
            bb_tcp_client_priv_health_report(false);
        }
        return rc;
    }
    if (inst->readable_pos < inst->readable_len) {
        size_t avail = inst->readable_len - inst->readable_pos;
        size_t n = (len < avail) ? len : avail;
        memcpy(buf, inst->readable_buf + inst->readable_pos, n);
        inst->readable_pos += n;
        *out_len = n;
        return BB_OK;
    }
#endif

    // No data queued: host stub has no real socket to select() on, so an
    // empty read is indistinguishable from "nothing arrived yet" — timeout.
    return BB_ERR_TIMEOUT;
}

bb_err_t bb_tcp_client_write(bb_tcp_client_t h, const uint8_t *buf, size_t len)
{
    if (!buf) return BB_ERR_INVALID_ARG;

    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst) return BB_ERR_INVALID_ARG;
    if (inst->state != BB_TCP_CLIENT_CONNECTED) return BB_ERR_INVALID_STATE;

#ifdef BB_TCP_CLIENT_TESTING
    if (inst->forced_io_result_set) {
        bb_err_t rc = inst->forced_io_result;
        inst->forced_io_result_set = false;
        if (rc != BB_OK && rc != BB_ERR_TIMEOUT) {
            bb_tcp_client_priv_health_report(false);
        }
        return rc;
    }
    size_t n = (len < sizeof(inst->last_write)) ? len : sizeof(inst->last_write);
    memcpy(inst->last_write, buf, n);
    inst->last_write_len = n;
    inst->write_count++;
#endif

    return BB_OK;  // host stub: "written" — no real socket
}

bb_err_t bb_tcp_client_poll_readable(bb_tcp_client_t h, uint32_t timeout_ms, bool *out_readable)
{
    (void)timeout_ms;
    if (!out_readable) return BB_ERR_INVALID_ARG;

    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst) return BB_ERR_INVALID_ARG;
    if (inst->state != BB_TCP_CLIENT_CONNECTED) {
        *out_readable = false;
        return BB_ERR_INVALID_STATE;
    }

#ifdef BB_TCP_CLIENT_TESTING
    *out_readable = (inst->readable_pos < inst->readable_len);
#else
    *out_readable = false;
#endif
    return BB_OK;
}

bb_err_t bb_tcp_client_close(bb_tcp_client_t h)
{
    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst) return BB_ERR_INVALID_ARG;

    inst->state = BB_TCP_CLIENT_DISCONNECTED;
#ifdef BB_TCP_CLIENT_TESTING
    inst->readable_len = 0;
    inst->readable_pos = 0;
#endif
    return BB_OK;
}

bb_tcp_client_state_t bb_tcp_client_get_state(bb_tcp_client_t h)
{
    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst) return BB_TCP_CLIENT_DISCONNECTED;
    return inst->state;
}

bb_err_t bb_tcp_client_destroy(bb_tcp_client_t h)
{
    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst) return BB_OK;  // NULL-safe / already-released, no-op

    bb_tcp_client_close(h);
    memset(inst, 0, sizeof(*inst));
    return BB_OK;
}

#ifdef BB_TCP_CLIENT_TESTING

void bb_tcp_client_test_reset(void)
{
    memset(s_pool, 0, sizeof(s_pool));
    bb_tcp_client_priv_reset_health_for_test();
}

void bb_tcp_client_test_inject_readable(bb_tcp_client_t h, const uint8_t *buf, size_t len)
{
    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst || !buf) return;

    size_t n = (len < sizeof(inst->readable_buf)) ? len : sizeof(inst->readable_buf);
    memcpy(inst->readable_buf, buf, n);
    inst->readable_len = n;
    inst->readable_pos = 0;
}

int bb_tcp_client_test_last_write(bb_tcp_client_t h, uint8_t *out, size_t out_cap)
{
    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst || !out) return -1;
    if (inst->write_count == 0) return -1;
    if (inst->last_write_len > out_cap) return -1;

    memcpy(out, inst->last_write, inst->last_write_len);
    return (int)inst->last_write_len;
}

int bb_tcp_client_test_write_count(bb_tcp_client_t h)
{
    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst) return 0;
    return inst->write_count;
}

void bb_tcp_client_test_force_connect_result(bb_tcp_client_t h, bb_err_t err)
{
    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst) return;
    inst->forced_connect_result = err;
    inst->forced_connect_result_set = true;
}

void bb_tcp_client_test_force_io_result(bb_tcp_client_t h, bb_err_t err)
{
    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst) return;
    inst->forced_io_result = err;
    inst->forced_io_result_set = true;
}

#endif /* BB_TCP_CLIENT_TESTING */
