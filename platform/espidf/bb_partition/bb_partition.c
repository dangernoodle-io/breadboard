#include "bb_partition.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include <string.h>

static const char *partition_type_str(esp_partition_type_t type)
{
    switch (type) {
    case ESP_PARTITION_TYPE_APP:  return "app";
    case ESP_PARTITION_TYPE_DATA: return "data";
    default:                      return "unknown";
    }
}

static const char *partition_subtype_str(esp_partition_type_t type,
                                         esp_partition_subtype_t subtype)
{
    if (type == ESP_PARTITION_TYPE_APP) {
        switch (subtype) {
        case ESP_PARTITION_SUBTYPE_APP_FACTORY: return "factory";
        case ESP_PARTITION_SUBTYPE_APP_OTA_0:   return "ota_0";
        case ESP_PARTITION_SUBTYPE_APP_OTA_1:   return "ota_1";
        case ESP_PARTITION_SUBTYPE_APP_OTA_2:   return "ota_2";
        case ESP_PARTITION_SUBTYPE_APP_OTA_3:   return "ota_3";
        case ESP_PARTITION_SUBTYPE_APP_OTA_4:   return "ota_4";
        case ESP_PARTITION_SUBTYPE_APP_OTA_5:   return "ota_5";
        case ESP_PARTITION_SUBTYPE_APP_OTA_6:   return "ota_6";
        case ESP_PARTITION_SUBTYPE_APP_OTA_7:   return "ota_7";
        case ESP_PARTITION_SUBTYPE_APP_OTA_8:   return "ota_8";
        case ESP_PARTITION_SUBTYPE_APP_OTA_9:   return "ota_9";
        case ESP_PARTITION_SUBTYPE_APP_OTA_10:  return "ota_10";
        case ESP_PARTITION_SUBTYPE_APP_OTA_11:  return "ota_11";
        case ESP_PARTITION_SUBTYPE_APP_OTA_12:  return "ota_12";
        case ESP_PARTITION_SUBTYPE_APP_OTA_13:  return "ota_13";
        case ESP_PARTITION_SUBTYPE_APP_OTA_14:  return "ota_14";
        case ESP_PARTITION_SUBTYPE_APP_OTA_15:  return "ota_15";
        case ESP_PARTITION_SUBTYPE_APP_TEST:    return "test";
        default:                                return "unknown";
        }
    }
    if (type == ESP_PARTITION_TYPE_DATA) {
        switch (subtype) {
        case ESP_PARTITION_SUBTYPE_DATA_OTA:      return "otadata";
        case ESP_PARTITION_SUBTYPE_DATA_PHY:      return "phy";
        case ESP_PARTITION_SUBTYPE_DATA_NVS:      return "nvs";
        case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: return "coredump";
        case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS: return "nvs_keys";
        case ESP_PARTITION_SUBTYPE_DATA_EFUSE_EM: return "efuse";
        default:                                  return "unknown";
        }
    }
    return "unknown";
}

static void fill_info(bb_partition_info_t *out,
                      const esp_partition_t *p,
                      const esp_partition_t *running,
                      const esp_partition_t *next_ota)
{
    strlcpy(out->label,   p->label, sizeof(out->label));
    strlcpy(out->type,    partition_type_str(p->type),            sizeof(out->type));
    strlcpy(out->subtype, partition_subtype_str(p->type, p->subtype), sizeof(out->subtype));
    out->offset   = p->address;
    out->size     = p->size;
    out->running  = (p == running);
    out->next_ota = (p == next_ota);
}

bb_err_t bb_partition_list(bb_partition_info_t *out, size_t cap, size_t *count)
{
    if (!out || !count) return BB_ERR_INVALID_ARG;

    const esp_partition_t *running  = esp_ota_get_running_partition();
    const esp_partition_t *next_ota = esp_ota_get_next_update_partition(NULL);

    size_t n = 0;
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p) {
            if (n < cap) {
                fill_info(&out[n], p, running, next_ota);
            }
            n++;
        }
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);

    *count = n;
    return BB_OK;
}

bb_err_t bb_partition_get_running(bb_partition_info_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return BB_ERR_NOT_FOUND;
    const esp_partition_t *next_ota = esp_ota_get_next_update_partition(NULL);
    fill_info(out, running, running, next_ota);
    return BB_OK;
}

bb_err_t bb_partition_get_next_ota(bb_partition_info_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
    const esp_partition_t *next_ota = esp_ota_get_next_update_partition(NULL);
    if (!next_ota) return BB_ERR_NOT_FOUND;
    const esp_partition_t *running = esp_ota_get_running_partition();
    fill_info(out, next_ota, running, next_ota);
    return BB_OK;
}
