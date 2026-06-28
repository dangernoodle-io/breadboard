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

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Install the event-forwarder queue in s_log_vprintf.
// Called once by bb_log_event.c during its init. Pass NULL to disable.
// Non-blocking enqueue (drop-on-full) so the IDF log mutex is never held long.
void bb_log_event_set_queue(QueueHandle_t q);

// Retrieve dropped-enqueue counter for the event forwarder queue.
uint32_t bb_log_event_dropped(void);
#endif
