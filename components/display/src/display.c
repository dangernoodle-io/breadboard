#include "display.h"

#include "esp_log.h"
#include "board.h"

static const char *TAG = "display";

bb_display_err_t bb_display_init(void)
{
    ESP_LOGW(TAG, "display stub: init (%dx%d %s)", PANEL_WIDTH, PANEL_HEIGHT, PANEL_NAME);
    return ESP_OK;
}

void bb_display_clear(uint16_t rgb565)
{
    (void)rgb565;
}

void bb_display_draw_text(int x, int y, const char *text)
{
    (void)x;
    (void)y;
    (void)text;
}

void bb_display_show_splash(const char *product, const char *version)
{
    ESP_LOGI(TAG, "splash: %s %s", product ? product : "", version ? version : "");
}

void bb_display_show_prov(const char *ap_ssid, const char *ap_pass)
{
    ESP_LOGI(TAG, "prov: ssid=%s pass=%s", ap_ssid ? ap_ssid : "", ap_pass ? ap_pass : "");
}

void bb_display_off(void)
{
}
