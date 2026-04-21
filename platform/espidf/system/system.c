#include "http_server.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void bb_system_get_version(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    const esp_app_desc_t *app = esp_app_get_description();
    const char *v = (app && app->version[0]) ? app->version : "unknown";
    strncpy(out, v, out_size - 1);
    out[out_size - 1] = '\0';
}

void bb_system_reboot(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
