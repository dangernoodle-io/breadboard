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
 * initializes panel with built-in vendor commands, sets up backlight PWM, and clears display.
 *
 * @return ESP_OK on success, ESP_ERR_* on failure
 */
bb_display_err_t bb_display_init(void);

/**
 * Clear display to a solid RGB565 color.
 *
 * @param rgb565 Color in RGB565 format (5R:6G:5B bits)
 */
void bb_display_clear(uint16_t rgb565);

/**
 * Draw text at specified coordinates using 8x8 bitmap font.
 * Text wraps to panel width; clips at panel bounds.
 *
 * @param x      Starting x coordinate
 * @param y      Starting y coordinate
 * @param text   Null-terminated string
 */
void bb_display_draw_text(int x, int y, const char *text);

/**
 * Show splash screen with product name and version.
 * Simple text-only layout; clears display first.
 *
 * @param product  Product name (e.g. "MyProduct")
 * @param version  Version string (e.g. "v1.2.3")
 */
void bb_display_show_splash(const char *product, const char *version);

/**
 * Show provisioning screen with AP SSID and password.
 * Simple text-only layout; clears display first.
 *
 * @param ap_ssid   AP SSID name
 * @param ap_pass   AP password
 */
void bb_display_show_prov(const char *ap_ssid, const char *ap_pass);

/**
 * Turn display off: disable backlight PWM and send panel sleep command.
 */
void bb_display_off(void);
