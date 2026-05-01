#include "bb_log.h"
#include "bb_core.h"
#include <string.h>

#ifdef CONFIG_BB_LOG_PANIC_CAPTURE

#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"

#define PANIC_MAGIC        0xBB10C7C7
#define BOOTS_SINCE_MAGIC  0xBB10C7C8

typedef struct {
    uint32_t magic;
    uint32_t length;
    uint32_t write_pos;
    uint32_t crc;
    char buf[CONFIG_BB_LOG_PANIC_BUF_SIZE];
} bb_log_panic_record_t;

static RTC_NOINIT_ATTR bb_log_panic_record_t s_panic_rec;
// RTC-backed boots-since-panic counter. Survives soft resets, lost on power
// cycle (alongside s_panic_rec). Magic-validated separately so cold-boot
// garbage doesn't read as a stale counter value.
static RTC_NOINIT_ATTR uint32_t s_boots_since;
static RTC_NOINIT_ATTR uint32_t s_boots_since_magic;
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

#ifdef CONFIG_BB_LOG_PANIC_COREDUMP
#include "esp_core_dump.h"

static bb_log_panic_summary_t s_summary;
static bool s_have_summary = false;

static void bb_log_panic_coredump_init(void)
{
    if (esp_core_dump_image_check() != ESP_OK) return;

    esp_core_dump_summary_t cd_summary;
    if (esp_core_dump_get_summary(&cd_summary) != ESP_OK) return;

    strncpy(s_summary.task_name, cd_summary.exc_task, sizeof(s_summary.task_name) - 1);
    s_summary.task_name[sizeof(s_summary.task_name) - 1] = '\0';
    s_summary.exc_pc = cd_summary.exc_pc;

#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
    // Xtensa: exc_cause is in ex_info.exc_cause, backtrace in exc_bt_info.bt/depth
    s_summary.exc_cause = cd_summary.ex_info.exc_cause;
    uint32_t depth = cd_summary.exc_bt_info.depth;
    if (depth > BB_LOG_PANIC_BACKTRACE_MAX) depth = BB_LOG_PANIC_BACKTRACE_MAX;
    s_summary.bt_count = depth;
    for (uint32_t i = 0; i < depth; i++) {
        s_summary.bt_addrs[i] = cd_summary.exc_bt_info.bt[i];
    }
#else
    // RISC-V (ESP32-C3, ESP32-C6, ESP32-H2, ESP32-P4, etc.):
    // mcause in ex_info.mcause; no on-device backtrace (stack dump only)
    s_summary.exc_cause = cd_summary.ex_info.mcause;
    s_summary.bt_count = 0;
#endif

    s_have_summary = true;
}
#endif /* CONFIG_BB_LOG_PANIC_COREDUMP */

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

    // Update boots-since-panic counter. RTC NOINIT is garbage on cold boot;
    // magic-check before trusting the value. Reset to 0 on (a) cold boot,
    // (b) fresh panic-class reset reason (we are now the post-panic boot).
    bool was_panic_boot = (reason == ESP_RST_PANIC || reason == ESP_RST_TASK_WDT ||
                           reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT ||
                           reason == ESP_RST_BROWNOUT);
    if (s_boots_since_magic != BOOTS_SINCE_MAGIC || was_panic_boot) {
        s_boots_since = 0;
        s_boots_since_magic = BOOTS_SINCE_MAGIC;
    } else if (s_boots_since < UINT32_MAX) {
        s_boots_since++;
    }

#ifdef CONFIG_BB_LOG_PANIC_COREDUMP
    bb_log_panic_coredump_init();
#endif

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
    s_boots_since = 0;
    s_boots_since_magic = BOOTS_SINCE_MAGIC;
#ifdef CONFIG_BB_LOG_PANIC_COREDUMP
    s_have_summary = false;
    esp_core_dump_image_erase();
#endif
}

uint32_t bb_log_panic_boots_since(void)
{
    bool have_panic = s_have_panic_log;
#ifdef CONFIG_BB_LOG_PANIC_COREDUMP
    have_panic = have_panic || s_have_summary;
#endif
    if (!have_panic) return 0;
    return (s_boots_since_magic == BOOTS_SINCE_MAGIC) ? s_boots_since : 0;
}

bool bb_log_panic_coredump_available(void)
{
#ifdef CONFIG_BB_LOG_PANIC_COREDUMP
    return s_have_summary;
#else
    return false;
#endif
}

bb_err_t bb_log_panic_coredump_get(bb_log_panic_summary_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
#ifdef CONFIG_BB_LOG_PANIC_COREDUMP
    if (!s_have_summary) return BB_ERR_NOT_FOUND;
    *out = s_summary;
    return BB_OK;
#else
    return BB_ERR_NOT_FOUND;
#endif
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

bool bb_log_panic_coredump_available(void)
{
    return false;
}

bb_err_t bb_log_panic_coredump_get(bb_log_panic_summary_t *out)
{
    (void)out;
    return BB_ERR_NOT_FOUND;
}

uint32_t bb_log_panic_boots_since(void)
{
    return 0;
}

#endif /* CONFIG_BB_LOG_PANIC_CAPTURE */
