#include "esp_log.h"
#include "nvs_flash.h"
#include "bb_nv.h"
#include "bb_log.h"
#include "bb_display.h"
#include "bb_prov_default_form.h"

static const char *TAG = "minimal";

void app_main(void) {
    nvs_flash_init();
    bb_nv_config_init();
    bb_log_stream_init();
    bb_display_init();
    bb_display_show_splash("minimal", "v0.0.0");

    // Verify bb_prov_default_form is linked
    (void)bb_prov_default_form_get();

    ESP_LOGI(TAG, "smoke boot ok");
}
