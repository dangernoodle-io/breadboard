#pragma once
#include <stdint.h>
#include "bb_core.h"

/* This backend uses the consumer's bb_hw board header for I²C pins:
 *   PIN_I2C_SDA, PIN_I2C_SCL
 * (Plus the optional PIN_OLED_RST if a hardware reset line is wired —
 * leave undefined or set to -1 to skip.) */

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"

/* Optional: hand the backend a pre-initialized I²C master bus instead
 * of letting it create one. Call BEFORE bb_display_init. Useful when
 * the bus is shared with another device (sensor, ASIC). NULL clears
 * the override and the backend will create its own bus. */
void bb_display_ssd1306_set_i2c_bus(i2c_master_bus_handle_t bus);
#endif
