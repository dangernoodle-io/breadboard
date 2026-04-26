#include "bb_nv.h"
#include "bb_mdns.h"
#include "bb_mdns_host_test_hooks.h"
#include "bb_log.h"

static const char *TAG = "bb_mdns";

/* Counters for host test assertions. */
static int s_announce_count = 0;
static int s_set_txt_count  = 0;

int bb_mdns_host_announce_count(void) { return s_announce_count; }
int bb_mdns_host_set_txt_count(void)  { return s_set_txt_count; }
void bb_mdns_host_reset(void)
{
    s_announce_count = 0;
    s_set_txt_count  = 0;
}

/* ESP_PLATFORM-guarded stubs — no timer coalescing on host.
 * Explicit bb_mdns_announce() is what's testable; timer-based coalesce
 * is ESP-IDF hardware behaviour and is verified by flashing. */
void bb_mdns_set_txt(const char *key, const char *value)
{
    if (!key || !value) return;
    s_set_txt_count++;
    bb_log_d(TAG, "set_txt stub: %s=%s", key, value);
}

void bb_mdns_announce(void)
{
    s_announce_count++;
    bb_log_d(TAG, "announce stub");
}

bb_err_t bb_mdns_browse_start(const char *service, const char *proto,
                              bb_mdns_peer_cb on_peer,
                              bb_mdns_peer_removed_cb on_removed,
                              void *ctx)
{
    if (!service || !proto) {
        return BB_ERR_INVALID_ARG;
    }
    bb_log_d(TAG, "browse_start stub: %s.%s", service, proto);
    return BB_OK;
}

bb_err_t bb_mdns_browse_stop(const char *service, const char *proto)
{
    if (!service || !proto) {
        return BB_ERR_INVALID_ARG;
    }
    bb_log_d(TAG, "browse_stop stub: %s.%s", service, proto);
    return BB_OK;
}
