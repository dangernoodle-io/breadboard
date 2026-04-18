#pragma once

#define BOARD_NAME "elecrow-p4-hmi7"

// MIPI-DSI interface (EK79007 bridge)
// CLK±
#define PIN_MIPI_DSI_CLK_P    38
#define PIN_MIPI_DSI_CLK_N    37
// D0±
#define PIN_MIPI_DSI_D0_P     40
#define PIN_MIPI_DSI_D0_N     39
// D1±
#define PIN_MIPI_DSI_D1_P     36
#define PIN_MIPI_DSI_D1_N     35
// REXT — termination resistor control
#define PIN_MIPI_REXT         34
// Backlight power enable
#define PIN_BL_PWR            29
// Backlight PWM control
#define PIN_BL_PWM            31

// Display panel configuration
#define PANEL_NAME            "EK79007"
#define PANEL_WIDTH           1024
#define PANEL_HEIGHT          600
#define PANEL_LANES           2
#define PANEL_LANE_SPEED_MBPS 900
#define DPI_CLK_MHZ           51
// Horizontal timing: hsync active period, back porch, front porch
#define PANEL_HSYNC_ACTIVE    70
#define PANEL_HSYNC_BACK      160
#define PANEL_HSYNC_FRONT     160
// Vertical timing: vsync active period, back porch, front porch
#define PANEL_VSYNC_ACTIVE    10
#define PANEL_VSYNC_BACK      23
#define PANEL_VSYNC_FRONT     12
#define PANEL_COLOR_FORMAT    "RGB565"

// LDO channel 3 for MIPI DPHY voltage
#define LDO_CHANNEL           3
#define LDO_VOLTAGE_MV        2500

// GT911 capacitive touchscreen (I2C1)
#define PIN_I2C1_SDA          45
#define PIN_I2C1_SCL          46
#define PIN_TOUCH_INT         42
#define PIN_TOUCH_RST         40
#define I2C1_BUS_SPEED_HZ     400000
#define I2C1_BUS_NUM          1
#define GT911_I2C_ADDR        0x5D

// ESP32-C6 companion (SDIO 4-bit mode)
#define PIN_SDIO_CLK          18
#define PIN_SDIO_CMD          19
#define PIN_SDIO_D0           14
#define PIN_SDIO_D1           15
#define PIN_SDIO_D2           16
#define PIN_SDIO_D3           17
#define PIN_C6_RST            11

// USB-Serial/JTAG interface
#define PIN_USB_D_PLUS        25
#define PIN_USB_D_MINUS       24

// SD card (SPI mode)
#define PIN_SD_CMD            44
#define PIN_SD_SCK            43
#define PIN_SD_D0             39

// Audio interface (I2S)
#define PIN_AUDIO_LRCLK       21
#define PIN_AUDIO_BCLK        22
#define PIN_AUDIO_SDATA       23
// Amplifier enable/power
#define PIN_AUDIO_AMP         30

// Boot button
#define PIN_BOOT_BTN          0
// Reset button
#define PIN_RESET_BTN         0  // typically shared with BOOT or dedicated

// Flash configuration
#define FLASH_SIZE_MB         16
#define FLASH_MODE            "DIO"

// PSRAM configuration
#define PSRAM_SIZE_MB         32
#define PSRAM_MODE            "QUAD"
