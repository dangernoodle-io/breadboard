#include "esp_log.h"
#include "nvs_flash.h"
#include "nv_config.h"
#include "log_stream.h"
#include "display.h"

static const char *TAG = "minimal";

void app_main(void) {
    nvs_flash_init();
    bsp_nv_config_init();
    bsp_log_stream_init();
    bsp_display_init();
    bsp_display_show_splash("minimal", "v0.0.0");
    ESP_LOGI(TAG, "smoke boot ok");
}
