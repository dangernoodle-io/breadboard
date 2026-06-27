#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Watchdog for ST_CONNECTING. If neither GOT_IP nor DISCONNECT arrives within
// this window, the task treats it as a stalled association and re-attempts.
// 30 s >> normal associate+DHCP (2–8 s), so false positives are rare.
#define WIFI_RECONN_CONNECTING_TIMEOUT_MS CONFIG_BB_WIFI_RECONN_CONNECTING_TIMEOUT_MS

// No-IP watchdog: how often ST_IDLE polls for the zombie state (L2-associated
// but no DHCP IP). Bridged from Kconfig; falls back to 60 s on host builds.
#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_WIFI_NO_IP_WATCHDOG_S
#    define WIFI_RECONN_NO_IP_WATCHDOG_MS ((uint32_t)(CONFIG_BB_WIFI_NO_IP_WATCHDOG_S) * 1000U)
#  endif
#endif
#ifndef WIFI_RECONN_NO_IP_WATCHDOG_MS
#define WIFI_RECONN_NO_IP_WATCHDOG_MS 60000U
#endif

// BB_WIFI_NO_IP_WATCHDOG_ENABLE: bridge Kconfig opt-in; default 0 on host.
#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_WIFI_NO_IP_WATCHDOG_ENABLE
#    define BB_WIFI_NO_IP_WATCHDOG_ENABLE 1
#  endif
#endif
#ifndef BB_WIFI_NO_IP_WATCHDOG_ENABLE
#define BB_WIFI_NO_IP_WATCHDOG_ENABLE 0
#endif

// BB_WIFI_INACTIVE_TIME_S: beacon-loss inactive time; bridged from Kconfig; falls back to 45 s.
#ifdef ESP_PLATFORM
#  ifdef CONFIG_BB_WIFI_INACTIVE_TIME_S
#    define BB_WIFI_INACTIVE_TIME_S CONFIG_BB_WIFI_INACTIVE_TIME_S
#  endif
#endif
#ifndef BB_WIFI_INACTIVE_TIME_S
#define BB_WIFI_INACTIVE_TIME_S 45U
#endif

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

// Notify the reconnect manager that the IP was lost while still associated.
// Calls wifi_reconn_policy_on_lost_ip and issues esp_wifi_disconnect() to
// drop the stale association, which fires WIFI_EVENT_STA_DISCONNECTED and
// drives the normal recovery path. Does NOT set s_self_disconnect.
// Safe to call from the WiFi event task context.
void wifi_reconn_on_lost_ip(void);

// Diagnostic counters (lock-free reads of manager-owned state).
uint32_t wifi_reconn_get_lost_ip_count(void);
int64_t  wifi_reconn_get_lost_ip_age_us(void);

// Set s_self_disconnect so the next WIFI_EVENT_STA_DISCONNECTED is absorbed
// without triggering the reconnect FSM. Call before esp_wifi_stop() in a
// controlled restart to absorb the synthetic disconnect it generates.
void wifi_reconn_absorb_next_disconnect(void);
