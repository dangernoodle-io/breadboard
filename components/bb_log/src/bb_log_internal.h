#pragma once

#include "bb_log.h"

/**
 * Internal backend implementation. Called by the portable bb_log_level_set wrapper
 * after registry update. Platform-specific: esp_log_level_set on ESP-IDF, no-op elsewhere.
 */
void _bb_log_level_set_backend(const char *tag, bb_log_level_t level);

/**
 * Host-only: reset registry for testing. Compiled out on ESP-IDF.
 */
#ifndef ESP_PLATFORM
void _bb_log_registry_reset(void);
#endif
