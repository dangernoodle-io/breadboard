// bb_transport_health — platform-shared implementation (host + espidf).
// Pure slot registry + observe-only authoritative-count guarantee.

#include "bb_transport_health.h"
#include "bb_clock.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define BB_TH_LOCK()   portENTER_CRITICAL(&s_lock)
#define BB_TH_UNLOCK() portEXIT_CRITICAL(&s_lock)
#else
#define BB_TH_LOCK()
#define BB_TH_UNLOCK()
#endif

typedef struct {
    bool                  used;
    bb_transport_class_t  cls;
    const char           *name;
    bool                  enabled;
    bool                  failing;
    uint64_t              last_ok_ms;
    uint32_t              fail_count;
    uint64_t              last_rx_ms;
    uint32_t              rx_count;
} bb_transport_health_slot_t;

static bb_transport_health_slot_t s_slots[BB_TRANSPORT_HEALTH_MAX_SLOTS];
static bool s_initialized = false;

static void ensure_init(void)
{
    if (s_initialized) return;
    BB_TH_LOCK();
    if (!s_initialized) {
        memset(s_slots, 0, sizeof(s_slots));
        s_initialized = true;
    }
    BB_TH_UNLOCK();
}

bool bb_transport_health_is_stale(uint64_t last_rx_ms, uint64_t now_ms, uint32_t threshold_s)
{
    uint64_t threshold_ms = (uint64_t)threshold_s * 1000ULL;
    if (now_ms <= last_rx_ms) return false;
    return (now_ms - last_rx_ms) > threshold_ms;
}

bb_err_t bb_transport_health_register(const char *name, bb_transport_class_t cls,
                                       bb_transport_handle_t *out)
{
    if (!name || !out) return BB_ERR_INVALID_ARG;
    *out = BB_TRANSPORT_HANDLE_INVALID;
    if (cls != BB_TRANSPORT_AUTHORITATIVE && cls != BB_TRANSPORT_INFERRED) {
        return BB_ERR_INVALID_ARG;
    }
    ensure_init();

    bb_err_t rc = BB_ERR_NO_SPACE;
    BB_TH_LOCK();
    for (int i = 0; i < BB_TRANSPORT_HEALTH_MAX_SLOTS; i++) {
        if (!s_slots[i].used) {
            s_slots[i].used = true;
            s_slots[i].cls = cls;
            s_slots[i].name = name;
            s_slots[i].enabled = true;
            s_slots[i].failing = false;
            s_slots[i].last_ok_ms = 0;
            s_slots[i].fail_count = 0;
            s_slots[i].last_rx_ms = 0;
            s_slots[i].rx_count = 0;
            *out = (bb_transport_handle_t)i;
            rc = BB_OK;
            break;
        }
    }
    BB_TH_UNLOCK();
    return rc;
}

static bool handle_valid(bb_transport_handle_t h)
{
    return h >= 0 && h < BB_TRANSPORT_HEALTH_MAX_SLOTS && s_slots[h].used;
}

bb_err_t bb_transport_health_set_enabled(bb_transport_handle_t h, bool enabled)
{
    ensure_init();
    bb_err_t rc = BB_ERR_INVALID_ARG;
    BB_TH_LOCK();
    if (handle_valid(h)) {
        s_slots[h].enabled = enabled;
        rc = BB_OK;
    }
    BB_TH_UNLOCK();
    return rc;
}

bb_err_t bb_transport_health_report(bb_transport_handle_t h, bool ok)
{
    ensure_init();
    uint64_t now = bb_clock_now_ms64();
    bb_err_t rc = BB_ERR_INVALID_ARG;
    BB_TH_LOCK();
    if (handle_valid(h) && s_slots[h].cls == BB_TRANSPORT_AUTHORITATIVE) {
        if (ok) {
            s_slots[h].failing = false;
            s_slots[h].last_ok_ms = now;
        } else {
            s_slots[h].failing = true;
            s_slots[h].fail_count++;
        }
        rc = BB_OK;
    }
    BB_TH_UNLOCK();
    return rc;
}

bb_err_t bb_transport_health_mark_activity(bb_transport_handle_t h)
{
    ensure_init();
    uint64_t now = bb_clock_now_ms64();
    bb_err_t rc = BB_ERR_INVALID_ARG;
    BB_TH_LOCK();
    if (handle_valid(h) && s_slots[h].cls == BB_TRANSPORT_INFERRED) {
        s_slots[h].last_rx_ms = now;
        s_slots[h].rx_count++;
        rc = BB_OK;
    }
    BB_TH_UNLOCK();
    return rc;
}

bb_err_t bb_transport_health_authoritative_counts(int *out_enabled, int *out_failing)
{
    if (!out_enabled || !out_failing) return BB_ERR_INVALID_ARG;
    ensure_init();

    int enabled = 0, failing = 0;
    BB_TH_LOCK();
    for (int i = 0; i < BB_TRANSPORT_HEALTH_MAX_SLOTS; i++) {
        if (!s_slots[i].used) continue;
        if (s_slots[i].cls != BB_TRANSPORT_AUTHORITATIVE) continue; // observe-only: INFERRED never counted
        if (!s_slots[i].enabled) continue;
        enabled++;
        if (s_slots[i].failing) failing++;
    }
    BB_TH_UNLOCK();

    *out_enabled = enabled;
    *out_failing = failing;
    return BB_OK;
}

size_t bb_transport_health_snapshot_all(bb_transport_health_snapshot_t *out, size_t max)
{
    if (!out || max == 0) return 0;
    ensure_init();

    uint64_t now_ms = bb_clock_now_ms64();
    size_t n = 0;
    BB_TH_LOCK();
    for (int i = 0; i < BB_TRANSPORT_HEALTH_MAX_SLOTS && n < max; i++) {
        if (!s_slots[i].used) continue;
        bb_transport_health_slot_t *s = &s_slots[i];
        out[n].name = s->name;
        out[n].cls = s->cls;
        out[n].enabled = s->enabled;
        out[n].last_ok_ms = s->last_ok_ms;
        out[n].fail_count = s->fail_count;
        out[n].last_rx_ms = s->last_rx_ms;
        out[n].rx_count = s->rx_count;
        if (s->cls == BB_TRANSPORT_INFERRED) {
            out[n].failing = bb_transport_health_is_stale(s->last_rx_ms, now_ms,
                                                            BB_TRANSPORT_HEALTH_INFERRED_STALE_S);
        } else {
            out[n].failing = s->failing;
        }
        n++;
    }
    BB_TH_UNLOCK();
    return n;
}

#ifdef BB_TRANSPORT_HEALTH_TESTING
void bb_transport_health_reset_for_test(void)
{
    BB_TH_LOCK();
    memset(s_slots, 0, sizeof(s_slots));
    s_initialized = true;
    BB_TH_UNLOCK();
}
#endif
