#pragma once

#include <stddef.h>

#ifdef ESP_PLATFORM

#include <stdbool.h>

// Initialize mDNS. Registers got-IP callback on bb_wifi. Idempotent.
void bb_mdns_init(void);

// Set mDNS hostname. Must be called after bb_mdns_init().
void bb_mdns_set_hostname(const char *hostname);

// Set mDNS service type (e.g. "_taipanminer"). Must be called before bb_mdns_init().
// Defaults to "_bsp" if not set.
void bb_mdns_set_service_type(const char *service_type);

// Set mDNS instance name (e.g. "TaipanMiner"). Must be called before bb_mdns_init().
// Defaults to "BSP Device" if not set.
void bb_mdns_set_instance_name(const char *instance_name);

// Check if mDNS has been started.
bool bb_mdns_started(void);

#endif /* ESP_PLATFORM */

// Sanitize and build RFC 1035-compliant hostname label: lowercase [a-z0-9], collapse/trim dashes, cap at 63 chars.
void bb_mdns_build_hostname(const char *prefix, const char *suffix, char *out, size_t out_size);
