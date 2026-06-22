#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Watchdog for ST_CONNECTING. If neither GOT_IP nor DISCONNECT arrives within
// this window, the task treats it as a stalled association and re-attempts.
// 30 s >> normal associate+DHCP (2–8 s), so false positives are rare.
#define WIFI_RECONN_CONNECTING_TIMEOUT_MS CONFIG_BB_WIFI_RECONN_CONNECTING_TIMEOUT_MS

// Start the reconnect manager task. Call once from wifi_connect_sta()
// AFTER the initial blocking connect succeeds. Idempotent.
void wifi_reconn_start(void);

// True once the manager task is running. The WiFi event handler uses
// this to decide whether to delegate disconnect/got-ip events.
bool wifi_reconn_is_active(void);

// Non-blocking notifiers. Post to the manager's queue and return.
// Safe to call from the WiFi event task context. Drop on full queue.
void wifi_reconn_on_disconnect(uint8_t reason);
void wifi_reconn_on_got_ip(void);

// Lock-free diagnostic reads of manager-owned state.
void wifi_reconn_get_disconnect(uint8_t *reason, int64_t *age_us);
int  wifi_reconn_get_retry_count(void);
void wifi_reconn_get_histogram(uint16_t *out, size_t len);
