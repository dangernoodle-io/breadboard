// bb_tcp_client — ESP-IDF backend: real connected TCP/TLS stream transport
// wrapping esp_transport (tcp_transport component). One esp_transport_handle_t
// per pooled instance, opened on connect() and torn down on close()/destroy()
// so a later connect() on the same handle starts from a clean transport (no
// state poisoning). DUMB TRANSPORT: no reconnect/backoff here — see
// bb_tcp_client.h.
#include "bb_tcp_client.h"
#include "bb_tcp_client_priv.h"
#include "bb_log.h"
#include "bb_clock.h"
#include "bb_str.h"
#include "bb_tls_creds.h"

#include <inttypes.h>
#include <string.h>

#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ssl.h"
#include "esp_crt_bundle.h"
#include "esp_tls_errors.h"

static const char *TAG = "bb_tcp_client";

typedef struct {
    bool                          in_use;
    bb_tcp_client_cfg_t           cfg;
    bb_tcp_client_state_t         state;
    esp_transport_handle_t        transport;
    bb_tcp_client_health_state_t  health;
    // TLS credentials resolved once at init() (B1-1390) via
    // bb_tcp_client_priv_resolve_tls_creds() and cached for the instance's
    // lifetime -- zeroed for a plaintext (cfg.tls=false) instance. Freed by
    // bb_tcp_client_destroy(); read (never re-resolved) by connect().
    bb_tls_creds_t                creds;
} bb_tcp_client_inst_t;

// Records inst's health.tls_error_code from `t`'s esp-tls last-error struct
// when the connection is TLS -- a no-op for a plain-TCP instance. Must be
// called BEFORE esp_transport_destroy(t) (the error handle is owned by the
// transport and is invalid once it is destroyed).
static void record_tls_error(bb_tcp_client_inst_t *inst, esp_transport_handle_t t)
{
    if (!inst->cfg.tls) return;
    esp_tls_error_handle_t eh = esp_transport_get_error_handle(t);
    if (!eh) return;
    bb_tcp_client_priv_health_set_tls_error(&inst->health, (int64_t)eh->esp_tls_error_code);
}

// Single-writer-per-instance invariant: each pooled instance is driven by
// exactly one caller task from init through destroy (no concurrent handle
// sharing) — matches bb_udp_client's documented invariant. No lock needed.
// The one exception is inst->health, which IS read from a separate
// diag/egress task by design -- see health.lock in bb_tcp_client_priv.h.
static bb_tcp_client_inst_t s_pool[BB_TCP_CLIENT_MAX_INSTANCES];

static bb_tcp_client_inst_t *inst_from_handle(bb_tcp_client_t h)
{
    bb_tcp_client_inst_t *inst = (bb_tcp_client_inst_t *)h;
    if (!inst || !inst->in_use) return NULL;
    return inst;
}

bb_err_t bb_tcp_client_init(const char *ns, const bb_tcp_client_cfg_t *cfg_or_null, bb_tcp_client_t *out)
{
    if (!ns || ns[0] == '\0') return BB_ERR_INVALID_ARG;
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
    // health.lock is instance-acquire-scoped (see bb_tcp_client_priv.h) --
    // init here, destroy() tears it down on release.
    pthread_mutex_init(&inst->health.lock, NULL);

    if (cfg_or_null) {
        inst->cfg = *cfg_or_null;
        bb_tcp_client_priv_save_to_nvs(ns, cfg_or_null);
    } else {
        bb_tcp_client_priv_load_from_nvs(ns, &inst->cfg);
    }

    // TLS credential resolution (B1-1390): a no-op when inst->cfg.tls is
    // false. Resolved once here (not per-connect()) and cached in
    // inst->creds -- see the struct comment above and
    // bb_tcp_client_priv_resolve_tls_creds()'s contract.
    bb_err_t creds_rc = bb_tcp_client_priv_resolve_tls_creds(ns, &inst->cfg, &inst->creds);
    if (creds_rc != BB_OK) {
        bb_log_e(TAG, "tls creds resolve failed: %d", creds_rc);
        pthread_mutex_destroy(&inst->health.lock);
        memset(inst, 0, sizeof(*inst));
        return creds_rc;
    }

    inst->in_use = true;
    inst->state = BB_TCP_CLIENT_DISCONNECTED;
    *out = (bb_tcp_client_t)inst;
    return BB_OK;
}

static void teardown_transport(bb_tcp_client_inst_t *inst)
{
    if (!inst->transport) return;
    esp_transport_close(inst->transport);
    esp_transport_destroy(inst->transport);
    inst->transport = NULL;
}

bb_err_t bb_tcp_client_connect(bb_tcp_client_t h)
{
    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst) return BB_ERR_INVALID_ARG;
    if (inst->state == BB_TCP_CLIENT_CONNECTED) return BB_OK;  // idempotent

    esp_transport_handle_t t = inst->cfg.tls ? esp_transport_ssl_init() : esp_transport_tcp_init();
    if (!t) {
        bb_log_e(TAG, "transport init failed");
        bb_tcp_client_priv_health_report(&inst->health, false);
        return BB_ERR_INVALID_STATE;
    }

    if (inst->cfg.tls) {
        // inst->creds was resolved once at init() (B1-1390) via
        // bb_tcp_client_priv_resolve_tls_creds() -- an explicit cfg PEM
        // pointer was passed through as bb_tls_creds' programmatic override,
        // so inst->creds.ca/cert/key already reflect the full precedence
        // (explicit cfg > NVS(ns) > embedded weak default). An empty resolve
        // (inst->creds.ca == NULL) is not an error -- fall through to the
        // ESP cert bundle exactly as before B1-1390.
        // bb_tls_creds' *_len fields are NUL-inclusive (see bb_tls_creds.c's
        // resolve_one()), but esp_transport_ssl_set_*_data() take the PEM
        // CONTENT length and add their own +1 for the NUL internally (see
        // esp-idf's tcp_transport/transport_ssl.c set_cert_data() family).
        // Passing the NUL-inclusive length here would tell esp-idf the
        // buffer is one byte longer than it actually is, an out-of-bounds
        // read that trips mbedtls' PEM-vs-DER autodetect. Subtract 1 to hand
        // over content length; the branches above guarantee *_len >= 1 here.
        if (inst->creds.ca) {
            esp_transport_ssl_set_cert_data(t, inst->creds.ca, (int)(inst->creds.ca_len - 1));
        } else {
            esp_transport_ssl_crt_bundle_attach(t, esp_crt_bundle_attach);
        }
        if (inst->creds.cert && inst->creds.key) {
            esp_transport_ssl_set_client_cert_data(t, inst->creds.cert, (int)(inst->creds.cert_len - 1));
            esp_transport_ssl_set_client_key_data(t, inst->creds.key, (int)(inst->creds.key_len - 1));
        }
    }

    uint32_t timeout_ms = inst->cfg.connect_timeout_ms ? inst->cfg.connect_timeout_ms : BB_TCP_CONNECT_TIMEOUT_MS;
    int rc = esp_transport_connect(t, inst->cfg.host, (int)inst->cfg.port, (int)timeout_ms);
    if (rc != 0) {
        bb_log_w(TAG, "connect %s:%" PRIu16 " failed (rc=%d)", inst->cfg.host, inst->cfg.port, rc);
        record_tls_error(inst, t);  // must run before destroy() invalidates the error handle
        esp_transport_destroy(t);
        bb_tcp_client_priv_health_report(&inst->health, false);
        return BB_ERR_INVALID_STATE;
    }

    inst->transport = t;
    inst->state = BB_TCP_CLIENT_CONNECTED;
    bb_tcp_client_priv_health_report(&inst->health, true);
    return BB_OK;
}

bb_err_t bb_tcp_client_read(bb_tcp_client_t h, uint8_t *buf, size_t len, size_t *out_len)
{
    if (!buf || !out_len) return BB_ERR_INVALID_ARG;
    *out_len = 0;

    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst) return BB_ERR_INVALID_ARG;
    if (inst->state != BB_TCP_CLIENT_CONNECTED) return BB_ERR_INVALID_STATE;

    uint32_t timeout_ms = inst->cfg.io_timeout_ms ? inst->cfg.io_timeout_ms : BB_TCP_IO_TIMEOUT_MS;
    int n = esp_transport_read(inst->transport, (char *)buf, (int)len, (int)timeout_ms);
    if (n > 0) {
        *out_len = (size_t)n;
        return BB_OK;
    }
    if (n == 0) {
        return BB_ERR_TIMEOUT;  // no data yet — not a failure signal
    }

    bb_log_w(TAG, "read error %d", n);
    record_tls_error(inst, inst->transport);
    bb_tcp_client_priv_health_report(&inst->health, false);
    return BB_ERR_INVALID_STATE;
}

bb_err_t bb_tcp_client_write(bb_tcp_client_t h, const uint8_t *buf, size_t len)
{
    if (!buf) return BB_ERR_INVALID_ARG;

    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst) return BB_ERR_INVALID_ARG;
    if (inst->state != BB_TCP_CLIENT_CONNECTED) return BB_ERR_INVALID_STATE;

    uint32_t timeout_ms = inst->cfg.io_timeout_ms ? inst->cfg.io_timeout_ms : BB_TCP_IO_TIMEOUT_MS;
    uint32_t deadline_ms = bb_clock_now_ms() + timeout_ms;
    size_t total = 0;

    while (total < len) {
        uint32_t now_ms = bb_clock_now_ms();
        int32_t remaining_ms = (int32_t)(deadline_ms - now_ms);
        if (remaining_ms <= 0) {
            bb_log_w(TAG, "write timed out after %u/%u bytes", (unsigned)total, (unsigned)len);
            record_tls_error(inst, inst->transport);
            bb_tcp_client_priv_health_report(&inst->health, false);
            return BB_ERR_INVALID_STATE;
        }

        int n = esp_transport_write(inst->transport, (const char *)buf + total,
                                     (int)(len - total), (int)remaining_ms);
        if (n < 0) {
            bb_log_w(TAG, "write error %d", n);
            record_tls_error(inst, inst->transport);
            bb_tcp_client_priv_health_report(&inst->health, false);
            return BB_ERR_INVALID_STATE;
        }
        total += (size_t)n;  // n == 0 (poll timeout) -> loop re-checks the deadline
    }

    return BB_OK;
}

bb_err_t bb_tcp_client_poll_readable(bb_tcp_client_t h, uint32_t timeout_ms, bool *out_readable)
{
    if (!out_readable) return BB_ERR_INVALID_ARG;

    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst) return BB_ERR_INVALID_ARG;
    if (inst->state != BB_TCP_CLIENT_CONNECTED) {
        *out_readable = false;
        return BB_ERR_INVALID_STATE;
    }

    int rc = esp_transport_poll_read(inst->transport, (int)timeout_ms);
    *out_readable = (rc > 0);
    return BB_OK;
}

bb_err_t bb_tcp_client_close(bb_tcp_client_t h)
{
    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst) return BB_ERR_INVALID_ARG;

    teardown_transport(inst);
    inst->state = BB_TCP_CLIENT_DISCONNECTED;
    bb_tcp_client_priv_health_close(&inst->health);
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

    teardown_transport(inst);
    bb_tls_creds_free(&inst->creds);  // safe on a zeroed (plaintext-instance) struct
    pthread_mutex_destroy(&inst->health.lock);
    memset(inst, 0, sizeof(*inst));
    return BB_OK;
}

bb_err_t bb_tcp_client_get_host_port(bb_tcp_client_t h, char *out_host, size_t out_host_cap, uint16_t *out_port)
{
    bool want_host = (out_host != NULL);
    if (want_host && out_host_cap == 0) return BB_ERR_INVALID_ARG;
    if (!want_host && !out_port) return BB_ERR_INVALID_ARG;

    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst) return BB_ERR_INVALID_ARG;

    // No lock needed: rests on the single-writer-per-instance invariant
    // above s_pool (bb_tcp_client_get_state()'s basis too). cfg is the
    // additional, stronger write-once-at-init guarantee (vs. inst->health).
    bb_err_t rc = BB_OK;
    if (want_host) {
        size_t src_len = bb_strlcpy(out_host, inst->cfg.host, out_host_cap);
        if (src_len >= out_host_cap) rc = BB_ERR_NO_SPACE;  // truncated but still NUL-terminated
    }
    if (out_port) *out_port = inst->cfg.port;
    return rc;
}

bb_err_t bb_tcp_client_health_fill(bb_tcp_client_t h, bb_tcp_client_health_snap_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;

    bb_tcp_client_inst_t *inst = inst_from_handle(h);
    if (!inst) return BB_ERR_INVALID_ARG;

    // Cross-task read: inst->health may be concurrently written by the FSM
    // task driving connect/read/write/close on this handle (see the
    // coherency comment on bb_tcp_client_health_state_t) -- copy all 4
    // fields under health.lock.
    pthread_mutex_lock(&inst->health.lock);
    out->connected      = inst->health.connected;
    out->last_ok_ms      = inst->health.last_ok_ms;
    out->fail_count      = (uint64_t)inst->health.fail_count;
    out->tls_error_code  = inst->health.tls_error_code;
    pthread_mutex_unlock(&inst->health.lock);
    return BB_OK;
}
