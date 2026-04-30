#include "bb_log.h"
#include "bb_core.h"

#ifdef CONFIG_BB_LOG_PANIC_CAPTURE

#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

#define PANIC_MAGIC 0xBB10C7C7

typedef struct {
    uint32_t magic;
    uint32_t length;
    uint32_t write_pos;
    uint32_t crc;
    char buf[CONFIG_BB_LOG_PANIC_BUF_SIZE];
} bb_log_panic_record_t;

static RTC_NOINIT_ATTR bb_log_panic_record_t s_panic_rec;
static bool s_have_panic_log = false;

static uint32_t bb_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320U : 0);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

static bb_err_t bb_log_panic_init(void)
{
    esp_reset_reason_t reason = esp_reset_reason();

    // Check magic + CRC to detect valid panic record
    bool valid_record = false;
    if (s_panic_rec.magic == PANIC_MAGIC && s_panic_rec.length <= CONFIG_BB_LOG_PANIC_BUF_SIZE) {
        uint32_t computed_crc = bb_crc32((const uint8_t *)s_panic_rec.buf, s_panic_rec.length);
        if (computed_crc == s_panic_rec.crc) {
            valid_record = true;
        }
    }

    // Determine if we should expose the panic log
    if (valid_record && (reason == ESP_RST_PANIC || reason == ESP_RST_TASK_WDT ||
                         reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT ||
                         reason == ESP_RST_BROWNOUT)) {
        s_have_panic_log = true;
    } else {
        s_have_panic_log = false;
    }

    // Reset magic for new boot's captures
    s_panic_rec.magic = 0;
    s_panic_rec.length = 0;
    s_panic_rec.write_pos = 0;

    return BB_OK;
}

// Register in early tier so we detect panic logs before normal boot proceeds
#include "bb_registry.h"
BB_REGISTRY_REGISTER_EARLY(bb_log_panic, bb_log_panic_init);

// Mirror write from bb_log_stream: called from s_log_vprintf when a line is written to ringbuf
void bb_log_panic_mirror_write(const char *data, size_t len)
{
    if (s_panic_rec.magic != PANIC_MAGIC) {
        // First write of this boot: initialize
        s_panic_rec.magic = PANIC_MAGIC;
        s_panic_rec.length = 0;
        s_panic_rec.write_pos = 0;
    }

    if (len == 0 || len > CONFIG_BB_LOG_PANIC_BUF_SIZE) return;

    // Circular write
    size_t remaining = len;
    size_t src_pos = 0;

    while (remaining > 0) {
        size_t space_to_end = CONFIG_BB_LOG_PANIC_BUF_SIZE - s_panic_rec.write_pos;
        size_t chunk = (remaining < space_to_end) ? remaining : space_to_end;

        memcpy(&s_panic_rec.buf[s_panic_rec.write_pos], &data[src_pos], chunk);
        s_panic_rec.write_pos = (s_panic_rec.write_pos + chunk) % CONFIG_BB_LOG_PANIC_BUF_SIZE;
        s_panic_rec.length = (s_panic_rec.length < CONFIG_BB_LOG_PANIC_BUF_SIZE) ?
                             (s_panic_rec.length + chunk) : CONFIG_BB_LOG_PANIC_BUF_SIZE;

        src_pos += chunk;
        remaining -= chunk;
    }

    s_panic_rec.crc = bb_crc32((const uint8_t *)s_panic_rec.buf, s_panic_rec.length);
}

bool bb_log_panic_available(void)
{
    return s_have_panic_log;
}

bb_err_t bb_log_panic_get(char *out, size_t *len_inout)
{
    if (!out || !len_inout || *len_inout == 0) {
        return BB_ERR_INVALID_ARG;
    }

    if (!s_have_panic_log) {
        return BB_ERR_NOT_FOUND;
    }

    size_t capacity = *len_inout;
    size_t to_copy = (s_panic_rec.length < capacity - 1) ? s_panic_rec.length : (capacity - 1);

    // Copy from the circular buffer in order (oldest to newest)
    // If we wrapped, start from write_pos; otherwise start from 0
    if (s_panic_rec.length == CONFIG_BB_LOG_PANIC_BUF_SIZE) {
        // Buffer is full and wrapped: copy write_pos to end, then 0 to write_pos
        size_t first_chunk = CONFIG_BB_LOG_PANIC_BUF_SIZE - s_panic_rec.write_pos;
        if (first_chunk > to_copy) first_chunk = to_copy;

        memcpy(out, &s_panic_rec.buf[s_panic_rec.write_pos], first_chunk);

        size_t second_chunk = to_copy - first_chunk;
        if (second_chunk > 0) {
            memcpy(&out[first_chunk], s_panic_rec.buf, second_chunk);
        }
    } else {
        // Buffer hasn't wrapped: copy from start
        memcpy(out, s_panic_rec.buf, to_copy);
    }

    out[to_copy] = '\0';
    *len_inout = to_copy;

    return BB_OK;
}

void bb_log_panic_clear(void)
{
    s_have_panic_log = false;
    s_panic_rec.magic = 0;
    s_panic_rec.length = 0;
    s_panic_rec.write_pos = 0;
    s_panic_rec.crc = 0;
}

#else

// Stubs when panic capture is disabled
bool bb_log_panic_available(void)
{
    return false;
}

bb_err_t bb_log_panic_get(char *out, size_t *len_inout)
{
    (void)out;
    (void)len_inout;
    return BB_ERR_NOT_FOUND;
}

void bb_log_panic_clear(void)
{
}

#endif /* CONFIG_BB_LOG_PANIC_CAPTURE */
