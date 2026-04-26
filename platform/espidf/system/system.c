#include "bb_http.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void bb_system_reboot(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
