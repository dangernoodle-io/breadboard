#pragma once

#ifdef ESP_PLATFORM

#include <stdbool.h>

// Initialize mDNS. Registers got-IP callback on bb_wifi. Idempotent.
void bb_mdns_init(void);

// Set mDNS hostname. Must be called after bb_mdns_init().
void bb_mdns_set_hostname(const char *hostname);

// Check if mDNS has been started.
bool bb_mdns_started(void);

#endif /* ESP_PLATFORM */
