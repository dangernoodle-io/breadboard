#pragma once
// Private forward declarations shared by bb_display_st7735.c and
// bb_display_st7789.c.  Not in any public include/ directory.
#include "bb_core.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include <stdint.h>

extern esp_lcd_panel_io_handle_t bb_display_st77xx_panel_io;
extern esp_lcd_panel_handle_t    bb_display_st77xx_panel;

bb_err_t bb_display_st77xx_init_bus(void);
void     bb_display_st77xx_clear(uint16_t);
void     bb_display_st77xx_blit(int16_t, int16_t, uint16_t, uint16_t, const uint16_t *);
void     bb_display_st77xx_off(void);
void     bb_display_st77xx_on(void);
