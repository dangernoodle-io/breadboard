#pragma once
#include <stdint.h>
#include "bb_core.h"

/* This backend uses the consumer's bb_hw board header for I²C pins:
 *   PIN_I2C_SDA, PIN_I2C_SCL
 * (Plus the optional PIN_OLED_RST if a hardware reset line is wired —
 * leave undefined or set to -1 to skip.) */
