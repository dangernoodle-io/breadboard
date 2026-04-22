#include "bb_log.h"

#include "esp_log.h"

void _bb_log_level_set_backend(const char *tag, bb_log_level_t level)
{
    if (!tag) return;

    static const esp_log_level_t map[] = {
        [BB_LOG_LEVEL_NONE] = ESP_LOG_NONE,
        [BB_LOG_LEVEL_ERROR] = ESP_LOG_ERROR,
        [BB_LOG_LEVEL_WARN] = ESP_LOG_WARN,
        [BB_LOG_LEVEL_INFO] = ESP_LOG_INFO,
        [BB_LOG_LEVEL_DEBUG] = ESP_LOG_DEBUG,
        [BB_LOG_LEVEL_VERBOSE] = ESP_LOG_VERBOSE,
    };

    if (level < 0 || level >= sizeof(map) / sizeof(map[0])) return;
    esp_log_level_set(tag, map[level]);
}
