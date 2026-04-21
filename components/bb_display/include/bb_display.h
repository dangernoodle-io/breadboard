#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
typedef esp_err_t bb_display_err_t;
#else
typedef int bb_display_err_t;
#endif

/**
 * Initialize the display (EK79007 MIPI-DSI RGB565 panel).
 * Configures LDO channel 3 for DPHY voltage, creates DSI bus with 2 lanes @ 900 Mbps,
 * initializes panel with built-in vendor commands, sets up backlight GPIO, then
 * initializes LVGL via esp_lvgl_port.
 *
 * @return ESP_OK on success, ESP_ERR_* on failure
 */
bb_display_err_t bb_display_init(void);

/**
 * Clear display to a solid color derived from an RGB565 value.
 *
 * @deprecated Prefer LVGL APIs directly (acquire lock, manipulate objects, release lock).
 *
 * @param rgb565 Color in RGB565 format (5R:6G:5B bits); expanded to RGB888 internally.
 *
 * @note Sets the active screen background color only. Does not remove existing widgets.
 */
void bb_display_clear(uint16_t rgb565);

/**
 * Show splash screen with product name and version.
 * Clears existing screen content and renders centered labels.
 *
 * @param product  Product name (e.g. "MyProduct")
 * @param version  Version string (e.g. "v1.2.3")
 */
void bb_display_show_splash(const char *product, const char *version);

/**
 * Show provisioning screen with AP SSID and password.
 * Clears existing screen content and renders stacked labels.
 *
 * @param ap_ssid   AP SSID name
 * @param ap_pass   AP password
 */
void bb_display_show_prov(const char *ap_ssid, const char *ap_pass);

/**
 * Turn display off: deinit LVGL port and tear down panel hardware.
 */
void bb_display_off(void);

#ifdef ESP_PLATFORM
#include "lvgl.h"

/**
 * Return the cached active LVGL screen for this display.
 *
 * Every call into LVGL from application code (including lv_timer callbacks
 * not running on the LVGL task) MUST be wrapped in bb_display_lock /
 * bb_display_unlock. Failing to hold the lock causes race conditions with
 * the LVGL port task.
 */
lv_obj_t *bb_display_screen(void);

/**
 * Acquire the LVGL port mutex.
 *
 * Every call into LVGL from application code (including lv_timer callbacks
 * not running on the LVGL task) MUST be wrapped in bb_display_lock /
 * bb_display_unlock. Failing to hold the lock causes race conditions with
 * the LVGL port task.
 *
 * @param timeout_ms Timeout in ms. 0 waits indefinitely.
 * @return true if lock was acquired, false on timeout.
 */
bool bb_display_lock(uint32_t timeout_ms);

/**
 * Release the LVGL port mutex acquired by bb_display_lock.
 */
void bb_display_unlock(void);

#endif /* ESP_PLATFORM */
