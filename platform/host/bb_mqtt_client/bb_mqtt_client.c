// bb_mqtt_client host stub — in-memory implementation for unit testing.
//
// bb_mqtt_client_init validates cfg and allocates a handle.
// bb_mqtt_client_publish records the last (and all) publish calls in a ring.
// bb_mqtt_client_is_connected returns a flag settable via bb_mqtt_client_host_set_connected.
// All operations return BB_OK; no real network IO occurs.
//
// "uri" (resume_default's NVS reload) round-trips through bb_config rather
// than bb_nv's generic KV forwarder (B1-756) — see s_mqtt_uri_field below.
#include "bb_mqtt_client.h"
#include "bb_config.h"
#include "bb_mqtt_client_nvs.h"
#include "bb_mqtt_client_reassemble.h"
#include "bb_mqtt_client_health.h"
#include "bb_str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BB_MQTT_CLIENT_HOST_PUB_CAP 32

// NVS key constants (host: mirrored from espidf backend). BB_MQTT_CLIENT_NVS_NS
// is owned by bb_mqtt_client (bb_mqtt_client_nvs.h). Declared early so the
// handle struct below can size its uri[] test-observability field against it.
#define BB_MQTT_CLIENT_URI_MAX 128

// Host stub has no CONFIG_BB_MQTT_CLIENT_RX_BUFFER_BYTES bridge (espidf-only
// Kconfig); mirror the Kconfig default so host-injected fragments behave
// the same as the on-device buffer sizing.
#define BB_MQTT_CLIENT_HOST_RX_BUFFER_BYTES 1024

#ifdef BB_MQTT_CLIENT_TESTING
static void *(*s_calloc_fn)(size_t, size_t) = NULL;
static void *bb_mqtt_client_calloc(size_t n, size_t sz)
{
    return s_calloc_fn ? s_calloc_fn(n, sz) : calloc(n, sz);
}
#else
#define bb_mqtt_client_calloc calloc
#endif

typedef struct {
    bb_mqtt_client_host_pub_t pubs[BB_MQTT_CLIENT_HOST_PUB_CAP];
    int                count;
    bool               connected;
    bool               tls;             // captured from cfg.tls at init time
    bool               ever_connected;  // set when connected goes true for first time
    uint32_t           reconnect_count;
    bb_mqtt_client_disc_t     disc_reason;
    bb_tls_fail_t      tls_fail;
    int                tls_error_code;
    int64_t            last_ok_ms;      // B1-1040: stamped on simulated connect success
    uint32_t           fail_count;      // B1-1040: bumped on simulated disconnect error
    bb_mqtt_client_msg_cb        msg_cb;    // B1-487: per-handle receive callback
    void                 *msg_ctx;
    bb_mqtt_client_reasm_state_t reasm;     // per-handle reassembly state (mirrors espidf)
    char               uri[BB_MQTT_CLIENT_URI_MAX];  // copy of cfg->uri, for host test observability
    int                enqueue_count;   // B1-834 PR-1: accepted (msg_id>=0) enqueue calls
    // B1-834 review CRITICAL-1/CRITICAL-2: mirrors esp-mqtt's real
    // esp_mqtt_client_enqueue()/outbox semantics (mqtt_client.c ~2280-2302)
    // so a host test can catch a regression of either fix -- store defaults
    // true (matches the espidf backend's fixed call site) and outbox_limit
    // defaults 0 (esp-mqtt's own unbounded default) unless a test opts in.
    bool               store;          // simulated store flag passed to esp_mqtt_client_enqueue
    // B1-834 review MEDIUM-1: PAYLOAD-ONLY LOWER BOUND, not a byte-exact
    // model. Real esp-mqtt's outbox_enqueue() (lib/mqtt_outbox.c:50-62) sets
    // item->len = message->len + message->remaining_len -- the FULL
    // serialized MQTT PUBLISH (topic + fixed/variable header + packet id
    // for qos>0 + payload), not just the payload bytes tracked here.
    // Deliberately not modeled more precisely (topic length + MQTT header
    // overhead varies with qos/topic length/remaining-length encoding) --
    // a boundary test passing here is a NECESSARY but not SUFFICIENT proof
    // that the same call sequence stays under the real device's outbox cap;
    // treat outbox_bytes as always <= the real on-device outbox occupancy.
    size_t             outbox_bytes;   // simulated cumulative accepted-but-undrained PAYLOAD size (lower bound -- see above)
    size_t             outbox_limit;   // 0 = unbounded (esp-mqtt default); test-settable cap
#ifdef BB_MQTT_CLIENT_TESTING
    bool               test_subscribe_fail;  // forces bb_mqtt_client_subscribe to fail once set
    int                test_enqueue_msg_id;  // B1-834 PR-1: forced msg_id for bb_mqtt_client_enqueue
#endif
} bb_mqtt_client_host_handle_t;

bb_err_t bb_mqtt_client_init(const bb_mqtt_client_cfg_t *cfg, bb_mqtt_client_t *out)
{
    if (!cfg || !out) return BB_ERR_INVALID_ARG;

    bb_mqtt_client_host_handle_t *h = bb_mqtt_client_calloc(1, sizeof(*h));
    if (!h) return BB_ERR_NO_SPACE;

    // Simulate "connected" by default so publish tests don't need to set flag.
    h->connected = true;
    h->tls       = cfg->tls;
    h->store     = true;  // matches the espidf backend's esp_mqtt_client_enqueue(store=true) call
    if (cfg->uri) {
        bb_strlcpy(h->uri, cfg->uri, sizeof(h->uri));
    }

    *out = h;
    return BB_OK;
}

bool bb_mqtt_client_is_tls(bb_mqtt_client_t handle)
{
    if (!handle) return false;
    return ((bb_mqtt_client_host_handle_t *)handle)->tls;
}

// Shared record-on-accept helper (firmware-review MEDIUM finding, B1-834):
// bb_mqtt_client_publish() and bb_mqtt_client_enqueue() both append an
// accepted message to the same ring, evicting the oldest entry once full --
// factored out on the second identical instance per the repo's reuse rule
// rather than re-hand-rolling it a second time. Returns the payload length
// actually recorded (post-truncation-aware caller uses this to size
// h->outbox_bytes); the caller supplies qos/retain since those are not
// derivable from payload/len alone.
static int mqtt_host_record_pub(bb_mqtt_client_host_handle_t *h, const char *topic,
                                 const char *payload, int len, int qos, bool retain)
{
    if (h->count >= BB_MQTT_CLIENT_HOST_PUB_CAP) {
        // Shift out oldest entry to make room.
        memmove(&h->pubs[0], &h->pubs[1],
                sizeof(h->pubs[0]) * (BB_MQTT_CLIENT_HOST_PUB_CAP - 1));
        h->count = BB_MQTT_CLIENT_HOST_PUB_CAP - 1;
    }

    bb_mqtt_client_host_pub_t *p = &h->pubs[h->count++];
    memset(p, 0, sizeof(*p));
    bb_strlcpy(p->topic, topic, sizeof(p->topic));
    int n = 0;
    if (payload) {
        // B1-834 review MEDIUM-2: matches esp-mqtt's own normalization
        // (mqtt_client.c:2177/2276: "if (len <= 0 && data != NULL) len =
        // strlen(data)") -- an explicit len=0 with non-NULL payload measures
        // via strlen, same as len<0. A prior version only auto-measured on
        // len<0, so len=0 host-side counted as 0 bytes while hardware
        // measured strlen(payload) -- a real divergence now that len feeds
        // the outbox boundary check below.
        n = (len <= 0) ? (int)strlen(payload) : len;
        if (n >= (int)sizeof(p->payload)) n = (int)sizeof(p->payload) - 1;
        memcpy(p->payload, payload, (size_t)n);
        p->payload[n] = '\0';
    }
    p->qos    = qos;
    p->retain = retain;

    return n;
}

// B1-834 review MEDIUM-2: shared payload-length normalization for the
// outbox boundary check in both publish() and enqueue() -- matches
// esp-mqtt's own normalization (mqtt_client.c:2177/2276: "if (len <= 0 &&
// data != NULL) len = strlen(data)"), i.e. `<= 0`, not `< 0`. Extracted on
// the second identical instance per the repo's reuse rule. Deliberately NOT
// truncated to BB_MQTT_CLIENT_HOST_PUB_CAP's payload[] bound (unlike
// mqtt_host_record_pub's internal `n`) -- the outbox math must reflect the
// full payload size, not the bounded copy kept for test observability.
static int mqtt_host_payload_len(const char *payload, int len)
{
    if (!payload) return 0;
    return (len <= 0) ? (int)strlen(payload) : len;
}

// B1-834 review HIGH-2: mirrors esp-mqtt's outbox-limit gate, which
// esp_mqtt_client_publish() ALSO applies -- unlike bb_mqtt_client_enqueue()'s
// check (unconditional on qos), publish()'s is gated on qos>0 (mqtt_client.c
// ~2181: "if (client->config->outbox_limit > 0 && qos > 0)"), since qos=0
// publishes are fire-and-forget and never enter the outbox via this path.
// outbox.limit is one client-wide config shared by publish()/enqueue(), so
// h->outbox_bytes/h->outbox_limit (set via bb_mqtt_client_host_set_outbox_limit)
// apply to both.
bb_err_t bb_mqtt_client_publish(bb_mqtt_client_t handle, const char *topic,
                          const char *payload, int len, int qos, bool retain)
{
    if (!handle || !topic) return BB_ERR_INVALID_ARG;
    bb_mqtt_client_host_handle_t *h = (bb_mqtt_client_host_handle_t *)handle;

    int payload_len = mqtt_host_payload_len(payload, len);

    if (qos > 0 && h->outbox_limit > 0 &&
        (size_t)payload_len + h->outbox_bytes > h->outbox_limit) {
        return BB_ERR_NO_SPACE;
    }

    mqtt_host_record_pub(h, topic, payload, len, qos, retain);
    if (qos > 0) {
        h->outbox_bytes += (size_t)payload_len;
    }

    return BB_OK;
}

// B1-834 PR-1: models esp_mqtt_client_enqueue()'s three outcomes via the
// sticky test_enqueue_msg_id hook (BB_MQTT_CLIENT_TESTING only). Outside
// testing builds the host stub always accepts (msg_id=0), matching
// bb_mqtt_client_publish()'s unconditional-BB_OK host behavior.
//
// B1-834 review HIGH-1: deliberately NO connected-state precondition here.
// Verified against the pinned esp-mqtt source (mqtt_client.c ~2263-2302):
// esp_mqtt_client_enqueue() has no connection-state check at all -- it
// rejects only on a NULL client, outbox-full (-2), and the store/qos quirk
// (-1). The espidf backend's own mqtt_acquire_client() guard checks
// `destroyed || client == NULL` (handle no longer exists), NOT "currently
// disconnected" -- enqueue-while-disconnected is the API's whole documented
// purpose (bb_mqtt_client.h: queue-while-offline / drain-on-reconnect). A
// prior version of this stub incorrectly reused h->connected (the flag
// bb_mqtt_client_host_set_connected() toggles) as a stand-in for
// "destroyed"; that was wrong and has been removed.
bb_err_t bb_mqtt_client_enqueue(bb_mqtt_client_t handle, const char *topic,
                          const char *payload, int len, int qos, bool retain)
{
    if (!handle || !topic) return BB_ERR_INVALID_ARG;
    bb_mqtt_client_host_handle_t *h = (bb_mqtt_client_host_handle_t *)handle;

    int payload_len = mqtt_host_payload_len(payload, len);

    // B1-834 review CRITICAL-2: mirrors esp-mqtt's outbox-limit gate
    // (mqtt_client.c: "if (client->config->outbox_limit > 0) { if (len +
    // outbox_get_size(client->outbox) > client->config->outbox_limit)
    // return -2; }"), checked BEFORE the accept/reject hook below, exactly
    // as esp-mqtt orders it. outbox_limit==0 means unbounded (esp-mqtt's own
    // default) and this check is skipped.
    if (h->outbox_limit > 0 &&
        (size_t)payload_len + h->outbox_bytes > h->outbox_limit) {
        return BB_ERR_NO_SPACE;
    }

    int msg_id = 0;
#ifdef BB_MQTT_CLIENT_TESTING
    msg_id = h->test_enqueue_msg_id;
#endif
    // B1-834 review CRITICAL-1: mirrors esp-mqtt's store/qos quirk
    // (mqtt_client.c ~2299: "if (ret == 0 && store == false) { messages with
    // qos=0 are not enqueued if not overridden by store_in_outbox -> indicate
    // as error; return -1; }") -- an accepted (msg_id==0) qos=0 enqueue is
    // forced to a generic reject when store is false, even though the
    // message really was accepted. The espidf backend always passes
    // store=true; this model exists so a regression back to store=false
    // shows up as a host-test failure (test_bb_mqtt_enqueue_qos0_store_false_*).
    if (qos == 0 && !h->store && msg_id == 0) {
        msg_id = -1;
    }
    if (msg_id == -2) return BB_ERR_NO_SPACE;
    if (msg_id == -1) return BB_ERR_VALIDATION;

    // Accepted -- record via the shared helper (see mqtt_host_record_pub
    // above) so bb_mqtt_client_host_last_pub()/bb_mqtt_client_host_pub_count()
    // also observe enqueued messages.
    mqtt_host_record_pub(h, topic, payload, len, qos, retain);
    h->enqueue_count++;
    h->outbox_bytes += (size_t)payload_len;

    return BB_OK;
}

bb_err_t bb_mqtt_client_subscribe(bb_mqtt_client_t handle, const char *topic, int qos)
{
    if (!handle || !topic) return BB_ERR_INVALID_ARG;
    (void)qos;
#ifdef BB_MQTT_CLIENT_TESTING
    bb_mqtt_client_host_handle_t *h = (bb_mqtt_client_host_handle_t *)handle;
    if (h->test_subscribe_fail) return BB_ERR_INVALID_STATE;
#endif
    return BB_OK;
}

// ---------------------------------------------------------------------------
// bb_mqtt_client_on_message / host inject (B1-487) — per-handle callback + reassembly
// state, mirroring the espidf backend (HIGH-2: no process-wide shared state).
// ---------------------------------------------------------------------------

bb_err_t bb_mqtt_client_on_message(bb_mqtt_client_t handle, bb_mqtt_client_msg_cb cb, void *ctx)
{
    if (!handle) return BB_ERR_INVALID_ARG;
    bb_mqtt_client_host_handle_t *h = (bb_mqtt_client_host_handle_t *)handle;

    if (cb && !h->reasm.buf) {
        h->reasm.buf = bb_mqtt_client_calloc(1, BB_MQTT_CLIENT_HOST_RX_BUFFER_BYTES);
        if (!h->reasm.buf) return BB_ERR_NO_SPACE;
        h->reasm.buf_cap = BB_MQTT_CLIENT_HOST_RX_BUFFER_BYTES;
        bb_mqtt_client_reasm_reset(&h->reasm);
    }
    h->msg_cb  = cb;
    h->msg_ctx = ctx;
    return BB_OK;
}

bool bb_mqtt_client_is_connected(bb_mqtt_client_t handle)
{
    if (!handle) return false;
    return ((bb_mqtt_client_host_handle_t *)handle)->connected;
}

bb_err_t bb_mqtt_client_get_stats(bb_mqtt_client_t handle, bb_mqtt_client_stats_t *out)
{
    if (!handle || !out) return BB_ERR_INVALID_ARG;
    bb_mqtt_client_host_handle_t *h = (bb_mqtt_client_host_handle_t *)handle;
    out->reconnect_count = h->reconnect_count;
    out->connected       = h->connected;
    out->disc_reason     = h->disc_reason;
    out->tls_fail        = h->tls_fail;
    out->tls_error_code  = h->tls_error_code;
    return BB_OK;
}

// B1-1040: no lock needed -- host stub is single-writer-per-instance,
// single-threaded tests only (mirrors bb_tcp_client's host backend
// convention; see bb_mqtt_client_health.c).
bb_err_t bb_mqtt_client_health_fill(bb_mqtt_client_t handle, bb_mqtt_client_health_snap_t *out)
{
    if (!handle || !out) return BB_ERR_INVALID_ARG;
    bb_mqtt_client_host_handle_t *h = (bb_mqtt_client_host_handle_t *)handle;
    out->connected      = h->connected;
    out->last_ok_ms     = h->last_ok_ms;
    out->fail_count     = (uint64_t)h->fail_count;
    out->tls_error_code = (int64_t)h->tls_error_code;
    return BB_OK;
}

bb_err_t bb_mqtt_client_destroy(bb_mqtt_client_t handle)
{
    if (!handle) return BB_OK;
    bb_mqtt_client_host_handle_t *h = (bb_mqtt_client_host_handle_t *)handle;
    // B1-1040: a caller-initiated destroy is a clean close, not a transport
    // failure -- the shared helper clears connected without touching
    // fail_count (see bb_mqtt_client_health.h). No lock needed here (host
    // is single-writer-per-instance).
    bb_mqtt_client_priv_health_close(&h->connected);
    free(h->reasm.buf);
    free(handle);
    return BB_OK;
}

bb_err_t bb_mqtt_client_stop(bb_mqtt_client_t *handle_p)
{
    if (!handle_p || !*handle_p) return BB_OK;
    bb_err_t rc = bb_mqtt_client_destroy(*handle_p);
    *handle_p = NULL;
    return rc;
}

// ---------------------------------------------------------------------------
// Default handle (host: settable via test hook; NULL by default)
// ---------------------------------------------------------------------------

static bb_mqtt_client_t s_default_handle = NULL;

bb_mqtt_client_t bb_mqtt_client_default(void)
{
    return s_default_handle;
}

// ---------------------------------------------------------------------------
// bb_mqtt_client_stop_default — host stub
//
// Stops and NULLs the default handle (mirrors ESP-IDF behavior).
// ---------------------------------------------------------------------------

bb_err_t bb_mqtt_client_stop_default(void)
{
    return bb_mqtt_client_stop(&s_default_handle);
}

// ---------------------------------------------------------------------------
// bb_mqtt_client_suspend_default / bb_mqtt_client_resume_default — host stubs
//
// Models BOTH suspend paths:
//
//   Default (stop-only=false):
//     - suspend: DESTROYS the handle (frees, NULLs s_default_handle).
//     - resume:  RECREATES a fresh handle (connected=true, pub ring cleared).
//
//   Stop-only (stop-only=true, set via bb_mqtt_client_host_set_stop_only under
//   BB_MQTT_CLIENT_TESTING):
//     - suspend: marks the handle as "stopped" (connected=false) but does NOT
//       free it; s_default_handle remains NON-NULL.
//     - resume:  marks the handle as "started" again (connected=true); same
//       handle pointer, no recreate.
//
// bb_mqtt_client_host_is_suspended_default() reports s_suspended for test assertions.
// ---------------------------------------------------------------------------

static bool s_suspended  = false;
static bool s_stop_only  = false;   // controlled by bb_mqtt_client_host_set_stop_only

// "uri" round-trips through bb_config (typed layer over bb_storage) rather
// than bb_nv's generic KV forwarder (B1-756, bb_nv dissolution epic B1-708)
// -- bb_config's STR encoding resolves to the SAME nvs_get_str call
// bb_nv_get_str made (both are thin forwarders to bb_storage_nvs), so this
// namespace/key/STR-typed on-flash format is byte-compatible with what this
// component previously read via bb_nv. Address matches
// bb_mqtt_client_espidf.c's s_mqtt_uri_field exactly.
static const bb_config_field_t s_mqtt_uri_field = {
    .id          = "mqtt.uri",
    .type        = BB_CONFIG_STR,
    .addr        = { .backend = "nvs", .ns_or_dir = BB_MQTT_CLIENT_NVS_NS, .key = "uri" },
    .max_len     = BB_MQTT_CLIENT_URI_MAX,
    .def         = { .str = "" },
    .has_default = true,
};

bb_err_t bb_mqtt_client_suspend_default(void)
{
    if (s_suspended) return BB_OK;   // idempotent

    if (s_stop_only) {
        // Stop-only: keep handle resident, just mark it stopped/disconnected.
        if (s_default_handle) {
            bb_mqtt_client_host_handle_t *h = (bb_mqtt_client_host_handle_t *)s_default_handle;
            h->connected = false;
        }
        // s_default_handle intentionally left NON-NULL.
    } else {
        // Full release: destroy the handle.
        if (s_default_handle) {
            bb_mqtt_client_destroy(s_default_handle);
            s_default_handle = NULL;
        }
    }

    s_suspended = true;
    return BB_OK;
}

bb_err_t bb_mqtt_client_resume_default(void)
{
    if (!s_suspended) return BB_OK;   // idempotent

    if (s_stop_only) {
        // Stop-only: restart the resident handle (connected=true, same pointer).
        if (s_default_handle) {
            bb_mqtt_client_host_handle_t *h = (bb_mqtt_client_host_handle_t *)s_default_handle;
            h->connected = true;
        }
    } else {
        // Full release: recreate a fresh handle from NVS (or fallback uri).
        char uri[BB_MQTT_CLIENT_URI_MAX] = {0};
        size_t uri_len = 0;
        bb_config_get_str(&s_mqtt_uri_field, uri, sizeof(uri), &uri_len);

        if (!uri[0]) {
            bb_strlcpy(uri, "mqtt://localhost:1883", sizeof(uri));
        }

        bb_mqtt_client_cfg_t cfg = {
            .uri = uri,
            .tls = false,
        };
        bb_err_t rc = bb_mqtt_client_init(&cfg, &s_default_handle);
        if (rc != BB_OK) {
            // Do NOT clear s_suspended — let caller retry.
            return rc;
        }
    }

    s_suspended = false;
    return BB_OK;
}

// ---------------------------------------------------------------------------
// Host test hooks
// ---------------------------------------------------------------------------

#ifdef BB_MQTT_CLIENT_TESTING

const bb_mqtt_client_host_pub_t *bb_mqtt_client_host_last_pub(bb_mqtt_client_t handle)
{
    if (!handle) return NULL;
    bb_mqtt_client_host_handle_t *h = (bb_mqtt_client_host_handle_t *)handle;
    if (h->count == 0) return NULL;
    return &h->pubs[h->count - 1];
}

int bb_mqtt_client_host_pub_count(bb_mqtt_client_t handle)
{
    if (!handle) return 0;
    return ((bb_mqtt_client_host_handle_t *)handle)->count;
}

void bb_mqtt_client_host_set_enqueue_msg_id(bb_mqtt_client_t handle, int msg_id)
{
    if (!handle) return;
    ((bb_mqtt_client_host_handle_t *)handle)->test_enqueue_msg_id = msg_id;
}

int bb_mqtt_client_host_enqueue_count(bb_mqtt_client_t handle)
{
    if (!handle) return 0;
    return ((bb_mqtt_client_host_handle_t *)handle)->enqueue_count;
}

void bb_mqtt_client_host_set_enqueue_store(bb_mqtt_client_t handle, bool store)
{
    if (!handle) return;
    ((bb_mqtt_client_host_handle_t *)handle)->store = store;
}

void bb_mqtt_client_host_set_outbox_limit(bb_mqtt_client_t handle, size_t limit_bytes)
{
    if (!handle) return;
    ((bb_mqtt_client_host_handle_t *)handle)->outbox_limit = limit_bytes;
}

// B1-834 review MEDIUM: h->outbox_bytes is otherwise monotonic (nothing else
// in this stub decrements it), which would make BB_ERR_NO_SPACE permanent
// once reached -- unlike real esp-mqtt, where outbox_get_size() shrinks as
// queued messages are sent/ACKed and freed. This hook simulates that drain
// so a test can prove the header's "retriable, back off and retry next
// tick" contract (bb_mqtt_client_enqueue()'s BB_ERR_NO_SPACE doc) actually
// holds. Floors at 0 -- draining more than is outstanding is a no-op past
// zero, not underflow.
void bb_mqtt_client_host_drain_outbox(bb_mqtt_client_t handle, size_t bytes)
{
    if (!handle) return;
    bb_mqtt_client_host_handle_t *h = (bb_mqtt_client_host_handle_t *)handle;
    h->outbox_bytes = (bytes >= h->outbox_bytes) ? 0 : (h->outbox_bytes - bytes);
}

const char *bb_mqtt_client_host_last_uri(bb_mqtt_client_t handle)
{
    if (!handle) return "";
    return ((bb_mqtt_client_host_handle_t *)handle)->uri;
}

void bb_mqtt_client_host_set_connected(bb_mqtt_client_t handle, bool connected)
{
    if (!handle) return;
    ((bb_mqtt_client_host_handle_t *)handle)->connected = connected;
}

void bb_mqtt_client_host_reset(bb_mqtt_client_t handle)
{
    if (!handle) return;
    bb_mqtt_client_host_handle_t *h = (bb_mqtt_client_host_handle_t *)handle;
    h->count                 = 0;
    h->connected             = true;
    h->ever_connected        = false;
    h->reconnect_count       = 0;
    h->disc_reason           = BB_MQTT_CLIENT_DISC_NONE;
    h->tls_fail              = BB_TLS_FAIL_NONE;
    h->tls_error_code        = 0;
    h->last_ok_ms            = 0;
    h->fail_count            = 0;
    h->enqueue_count         = 0;
    h->test_enqueue_msg_id   = 0;
    h->store                 = true;
    h->outbox_bytes          = 0;
    h->outbox_limit          = 0;
}

// ---------------------------------------------------------------------------
// Per-instance health test hooks (B1-1040)
// ---------------------------------------------------------------------------

void bb_mqtt_client_host_simulate_connect_success(bb_mqtt_client_t handle)
{
    if (!handle) return;
    bb_mqtt_client_host_handle_t *h = (bb_mqtt_client_host_handle_t *)handle;
    h->connected      = true;
    h->ever_connected = true;
    h->last_ok_ms     = bb_mqtt_client_priv_now_ms64();
}

void bb_mqtt_client_host_simulate_disconnect_error(bb_mqtt_client_t handle)
{
    if (!handle) return;
    bb_mqtt_client_host_handle_t *h = (bb_mqtt_client_host_handle_t *)handle;
    if (h->ever_connected) {
        h->reconnect_count++;
    }
    h->connected = false;
    h->fail_count++;  // unconditional -- see bb_mqtt_client.h's reporting policy
}

// Thin wrapper over the shared bb_mqtt_client_priv_health_close() helper --
// the SAME function both backends' bb_mqtt_client_destroy() call -- so this
// test hook exercises the real clean-close mutation, not a divergent
// stand-in (firmware-review finding, B1-1040).
void bb_mqtt_client_host_simulate_clean_close(bb_mqtt_client_t handle)
{
    if (!handle) return;
    bb_mqtt_client_host_handle_t *h = (bb_mqtt_client_host_handle_t *)handle;
    bb_mqtt_client_priv_health_close(&h->connected);
}

void bb_mqtt_client_host_simulate_reconnect(bb_mqtt_client_t handle)
{
    if (!handle) return;
    bb_mqtt_client_host_handle_t *h = (bb_mqtt_client_host_handle_t *)handle;
    if (!h->ever_connected) {
        h->ever_connected = true;
    }
    h->reconnect_count++;
}

void bb_mqtt_client_host_set_disc_reason(bb_mqtt_client_t handle, bb_mqtt_client_disc_t reason)
{
    if (!handle) return;
    ((bb_mqtt_client_host_handle_t *)handle)->disc_reason = reason;
}

void bb_mqtt_client_host_set_tls_fail(bb_mqtt_client_t handle, bb_tls_fail_t fail)
{
    if (!handle) return;
    ((bb_mqtt_client_host_handle_t *)handle)->tls_fail = fail;
}

void bb_mqtt_client_host_set_tls_error_code(bb_mqtt_client_t handle, int code)
{
    if (!handle) return;
    ((bb_mqtt_client_host_handle_t *)handle)->tls_error_code = code;
}

void bb_mqtt_client_default_set(bb_mqtt_client_t h)
{
    s_default_handle = h;
}

bool bb_mqtt_client_host_is_suspended_default(void)
{
    return s_suspended;
}

void bb_mqtt_client_host_set_stop_only(bool stop_only)
{
    s_stop_only = stop_only;
}

void bb_mqtt_client_set_calloc(void *(*fn)(size_t, size_t))
{
    s_calloc_fn = fn;
}

void bb_mqtt_client_host_inject_message(bb_mqtt_client_t handle, const char *topic,
                                  const void *payload, size_t len)
{
    bb_mqtt_client_host_inject_fragment(handle, topic, len, 0, payload, len);
}

void bb_mqtt_client_host_inject_fragment(bb_mqtt_client_t handle, const char *topic,
                                   size_t total_len, size_t offset,
                                   const void *data, size_t data_len)
{
    if (!handle || !topic) return;
    bb_mqtt_client_host_handle_t *h = (bb_mqtt_client_host_handle_t *)handle;
    // The `!h->reasm.buf` half is defensive: the only assignment site for
    // h->msg_cb (bb_mqtt_client_on_message) always allocates reasm.buf
    // BEFORE setting msg_cb (and returns early, leaving msg_cb untouched, on
    // alloc failure) -- so msg_cb non-NULL provably implies buf non-NULL.
    if (!h->msg_cb || !h->reasm.buf) return;  // LCOV_EXCL_BR_LINE -- defensive, invariant above

    // Bound-copy the topic through a fixed buffer mirroring
    // BB_MQTT_CLIENT_SUB_TOPIC_MAX-style truncation, for host/device parity — the
    // caller's topic pointer is otherwise unbounded (LOW fix).
    char topic_bounded[BB_MQTT_CLIENT_REASM_TOPIC_MAX];
    snprintf(topic_bounded, sizeof(topic_bounded), "%s", topic);

    bool complete = bb_mqtt_client_reasm_step(&h->reasm, topic_bounded,
                                        strlen(topic_bounded),
                                        total_len, offset, data, data_len);
    if (complete) {
        h->msg_cb(h->reasm.topic, h->reasm.buf, h->reasm.len, h->msg_ctx);
    }
}

void bb_mqtt_client_host_set_subscribe_fail(bb_mqtt_client_t handle, bool fail)
{
    if (!handle) return;
    ((bb_mqtt_client_host_handle_t *)handle)->test_subscribe_fail = fail;
}

#endif /* BB_MQTT_CLIENT_TESTING */
