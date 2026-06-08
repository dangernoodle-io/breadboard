#include "bb_diag.h"
#include "bb_core.h"
#include "bb_log.h"
#include "bb_nv.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include <string.h>

#define BB_DIAG_NV_NS        "bb_diag"
#define BB_DIAG_NV_KEY_RST   "reset_count"
/* 4-byte fingerprint of the running app's ELF SHA256; used to detect firmware
 * changes and auto-reset the abnormal-reset counter on new deploys/OTA. */
#define BB_DIAG_NV_KEY_APPFP "app_fp"

/* Derive a stable 32-bit fingerprint from the running app's ELF SHA256.
 * Uses the first 4 bytes of the raw SHA256 (little-endian uint32). Collision
 * probability is negligible for change-detection between consecutive builds. */
static uint32_t bb_diag_running_app_fp(void)
{
    const uint8_t *sha = esp_app_get_description()->app_elf_sha256;
    return ((uint32_t)sha[0])
         | ((uint32_t)sha[1] << 8)
         | ((uint32_t)sha[2] << 16)
         | ((uint32_t)sha[3] << 24);
}

static uint32_t s_abnormal_reset_count = 0;

uint32_t bb_diag_abnormal_reset_count(void) { return s_abnormal_reset_count; }

void bb_diag_abnormal_reset_count_clear(void)
{
    s_abnormal_reset_count = 0;
    bb_nv_set_u32(BB_DIAG_NV_NS, BB_DIAG_NV_KEY_RST, 0);
}

#ifdef CONFIG_BB_DIAG_PANIC_CAPTURE

#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"

#define PANIC_MAGIC        0xBB10C7C7
#define BOOTS_SINCE_MAGIC  0xBB10C7C8

typedef struct {
    uint32_t magic;
    uint32_t length;
    uint32_t write_pos;
    /* Legacy: per-write CRC, no longer maintained. Field kept so the on-RTC
     * struct layout doesn't shift for projects that mix-and-match BB
     * versions across reboots; never written, never read. */
    uint32_t crc_unused;
    char buf[CONFIG_BB_DIAG_PANIC_BUF_SIZE];
} bb_diag_panic_record_t;

static RTC_NOINIT_ATTR bb_diag_panic_record_t s_panic_rec;
// RTC-backed boots-since-panic counter. Survives soft resets, lost on power
// cycle (alongside s_panic_rec). Magic-validated separately so cold-boot
// garbage doesn't read as a stale counter value.
static RTC_NOINIT_ATTR uint32_t s_boots_since;
static RTC_NOINIT_ATTR uint32_t s_boots_since_magic;
static bool s_have_panic_log = false;

// Serve buffer: ordered copy of the crash log, preserved before the RTC
// buffer is reset for the recovery boot. Plain .bss — repopulated each boot
// from RTC, no need to survive resets. bb_diag_panic_get serves from here.
static char   s_panic_log_serve[CONFIG_BB_DIAG_PANIC_BUF_SIZE];
static size_t s_panic_log_serve_len = 0;

#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
#include "esp_core_dump.h"
#include "esp_partition.h"

static bb_diag_panic_summary_t s_summary;
static bool s_have_summary = false;

static void bb_diag_panic_coredump_init(void)
{
    if (esp_core_dump_image_check() != ESP_OK) return;

    esp_core_dump_summary_t cd_summary;
    if (esp_core_dump_get_summary(&cd_summary) != ESP_OK) return;

    strncpy(s_summary.task_name, cd_summary.exc_task, sizeof(s_summary.task_name) - 1);
    s_summary.task_name[sizeof(s_summary.task_name) - 1] = '\0';
    bb_diag_scrub_text(s_summary.task_name);
    s_summary.exc_pc = cd_summary.exc_pc;

#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
    // Xtensa: exc_cause is in ex_info.exc_cause, backtrace in exc_bt_info.bt/depth
    s_summary.exc_cause = cd_summary.ex_info.exc_cause;
    uint32_t depth = cd_summary.exc_bt_info.depth;
    if (depth > BB_DIAG_PANIC_BACKTRACE_MAX) depth = BB_DIAG_PANIC_BACKTRACE_MAX;
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

    s_summary.panic_reason[0] = '\0';
    if (esp_core_dump_get_panic_reason(s_summary.panic_reason, sizeof(s_summary.panic_reason)) == ESP_OK) {
        bb_diag_scrub_text(s_summary.panic_reason);
    } else {
        s_summary.panic_reason[0] = '\0';   /* defensive: clear on partial fill */
    }

    /* Copy ELF SHA256: cd_summary.app_elf_sha256 is a NUL-terminated hex string
     * of length APP_ELF_SHA256_SZ (= CONFIG_APP_RETRIEVE_LEN_ELF_SHA + 1). */
    strncpy(s_summary.app_sha256, (const char *)cd_summary.app_elf_sha256,
            sizeof(s_summary.app_sha256) - 1);
    s_summary.app_sha256[sizeof(s_summary.app_sha256) - 1] = '\0';

    s_have_summary = true;
}
#endif /* CONFIG_BB_DIAG_PANIC_COREDUMP */

// Forward decl: tap entry called from bb_log_stream
void bb_diag_panic_capture_write(const char *data, size_t len);

/**
 * Copy the circular panic buffer (oldest-to-newest) into a flat output buffer.
 * Pure function — no global state. Used both at boot-time preserve and in tests.
 *
 * @param buf       Source circular buffer of size buf_size.
 * @param buf_size  Total capacity of the circular buffer.
 * @param length    Number of valid bytes in buf (clamped to buf_size).
 * @param write_pos Current write cursor (next byte to overwrite when full).
 * @param out       Destination buffer; will be NUL-terminated on return.
 * @param out_cap   Capacity of out (including space for NUL terminator).
 * @return Number of bytes written (excluding NUL).
 */
size_t bb_diag_panic_order_copy(const char *buf, size_t buf_size,
                                 size_t length, size_t write_pos,
                                 char *out, size_t out_cap)
{
    if (!buf || !out || buf_size == 0 || out_cap == 0) {
        if (out && out_cap > 0) out[0] = '\0';
        return 0;
    }

    size_t to_copy = (length < out_cap - 1) ? length : (out_cap - 1);

    if (length == buf_size) {
        // Buffer full and wrapped: oldest byte is at write_pos
        size_t first_chunk = buf_size - write_pos;
        if (first_chunk > to_copy) first_chunk = to_copy;

        memcpy(out, &buf[write_pos], first_chunk);

        size_t second_chunk = to_copy - first_chunk;
        if (second_chunk > 0) {
            memcpy(&out[first_chunk], buf, second_chunk);
        }
    } else {
        // Not wrapped: oldest byte is at index 0
        memcpy(out, buf, to_copy);
    }

    out[to_copy] = '\0';
    return to_copy;
}

static bb_err_t bb_diag_panic_init(void)
{
    esp_reset_reason_t reason = esp_reset_reason();

    // Magic + length-bound check is sufficient to detect a valid panic
    // record. The previous CRC validation was paired with a per-write CRC
    // (recomputed on every log line) that stalled mining_hw on classic
    // ESP32 hard enough to trip the task watchdog (B1-227). A panic that
    // interrupts a write leaves at worst a few bytes of garbage at the
    // buffer tail — acceptable for a best-effort post-mortem log.
    bool valid_record = (s_panic_rec.magic == PANIC_MAGIC &&
                         s_panic_rec.length <= CONFIG_BB_DIAG_PANIC_BUF_SIZE);

    // Determine if we should expose the panic log
    if (valid_record && (reason == ESP_RST_PANIC || reason == ESP_RST_TASK_WDT ||
                         reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT ||
                         reason == ESP_RST_BROWNOUT)) {
        s_have_panic_log = true;
        // Preserve the crash log into the serve buffer BEFORE resetting the
        // RTC record — the tap re-armed below will start writing the recovery
        // boot's logs into s_panic_rec, overwriting the crash boot's data.
        // bb_diag_panic_get serves from s_panic_log_serve, not s_panic_rec.
        s_panic_log_serve_len = bb_diag_panic_order_copy(
            s_panic_rec.buf,
            CONFIG_BB_DIAG_PANIC_BUF_SIZE,
            s_panic_rec.length,
            s_panic_rec.write_pos,
            s_panic_log_serve,
            sizeof(s_panic_log_serve));
    } else {
        s_have_panic_log = false;
        s_panic_log_serve_len = 0;
    }

    // Reset magic for new boot's captures
    s_panic_rec.magic = 0;
    s_panic_rec.length = 0;
    s_panic_rec.write_pos = 0;

    // Arm the log-stream tap as early as possible — immediately after the RTC
    // buffer is clean — so any crash that occurs during the remainder of this
    // init (NVS counter, coredump summary) or in a later EARLY component is
    // captured. bb_log_stream_init() is idempotent; if bb_log_stream ran first
    // in the EARLY walker this is a no-op. If bb_diag_panic runs first it boots
    // the stream right now, before any other work below.
    bb_log_stream_init();
    bb_log_stream_set_tap(bb_diag_panic_capture_write);

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

    // NVS-backed abnormal-reset counter — "abnormal resets since this firmware
    // was deployed"; auto-resets when the running app SHA fingerprint changes.
    {
        uint32_t stored_fp = 0;
        uint32_t stored_count = 0;
        bb_nv_get_u32(BB_DIAG_NV_NS, BB_DIAG_NV_KEY_APPFP, &stored_fp, 0);
        bb_nv_get_u32(BB_DIAG_NV_NS, BB_DIAG_NV_KEY_RST, &stored_count, 0);
        uint32_t running_fp = bb_diag_running_app_fp();
        bb_diag_reset_result_t r = bb_diag_reset_decision(stored_fp, running_fp,
                                                           stored_count, was_panic_boot);
        s_abnormal_reset_count = r.new_count;
        bb_nv_batch_t batch;
        if (bb_nv_batch_begin(&batch, BB_DIAG_NV_NS) == BB_OK) {
            bb_nv_batch_set_u32(&batch, BB_DIAG_NV_KEY_RST, r.new_count);
            if (r.store_fp) {
                bb_nv_batch_set_u32(&batch, BB_DIAG_NV_KEY_APPFP, running_fp);
            }
            bb_nv_batch_commit(&batch);
        }
    }

#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
    bb_diag_panic_coredump_init();
#endif

    return BB_OK;
}

// Register in early tier so we detect panic logs before normal boot proceeds
#include "bb_registry.h"
BB_REGISTRY_REGISTER_EARLY(bb_diag_panic, bb_diag_panic_init);

// Tap entry called from bb_log_stream's vprintf hook for every line
void bb_diag_panic_capture_write(const char *data, size_t len)
{
    if (s_panic_rec.magic != PANIC_MAGIC) {
        // First write of this boot: initialize
        s_panic_rec.magic = PANIC_MAGIC;
        s_panic_rec.length = 0;
        s_panic_rec.write_pos = 0;
    }

    if (len == 0 || len > CONFIG_BB_DIAG_PANIC_BUF_SIZE) return;

    // Circular write
    size_t remaining = len;
    size_t src_pos = 0;

    while (remaining > 0) {
        size_t space_to_end = CONFIG_BB_DIAG_PANIC_BUF_SIZE - s_panic_rec.write_pos;
        size_t chunk = (remaining < space_to_end) ? remaining : space_to_end;

        memcpy(&s_panic_rec.buf[s_panic_rec.write_pos], &data[src_pos], chunk);
        s_panic_rec.write_pos = (s_panic_rec.write_pos + chunk) % CONFIG_BB_DIAG_PANIC_BUF_SIZE;
        s_panic_rec.length = (s_panic_rec.length < CONFIG_BB_DIAG_PANIC_BUF_SIZE) ?
                             (s_panic_rec.length + chunk) : CONFIG_BB_DIAG_PANIC_BUF_SIZE;

        src_pos += chunk;
        remaining -= chunk;
    }

    /* No CRC maintenance — see init() for the rationale (B1-227). */
}

bool bb_diag_panic_available(void)
{
    return s_have_panic_log;
}

bb_err_t bb_diag_panic_get(char *out, size_t *len_inout)
{
    if (!out || !len_inout || *len_inout == 0) {
        return BB_ERR_INVALID_ARG;
    }

    if (!s_have_panic_log) {
        return BB_ERR_NOT_FOUND;
    }

    // Serve from the pre-ordered serve buffer (populated at init from the crash
    // boot's RTC record, before the RTC buffer was reset for the recovery boot).
    size_t capacity = *len_inout;
    size_t to_copy = (s_panic_log_serve_len < capacity - 1)
                         ? s_panic_log_serve_len
                         : (capacity - 1);

    memcpy(out, s_panic_log_serve, to_copy);
    out[to_copy] = '\0';
    *len_inout = to_copy;

    return BB_OK;
}

void bb_diag_panic_clear(void)
{
    s_have_panic_log = false;
    s_panic_log_serve_len = 0;
    s_panic_rec.magic = 0;
    s_panic_rec.length = 0;
    s_panic_rec.write_pos = 0;
    s_panic_rec.crc_unused = 0;
    s_boots_since = 0;
    s_boots_since_magic = BOOTS_SINCE_MAGIC;
#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
    s_have_summary = false;
    esp_core_dump_image_erase();
#endif
}

uint32_t bb_diag_panic_boots_since(void)
{
    bool have_panic = s_have_panic_log;
#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
    have_panic = have_panic || s_have_summary;
#endif
    if (!have_panic) return 0;
    return (s_boots_since_magic == BOOTS_SINCE_MAGIC) ? s_boots_since : 0;
}

bool bb_diag_panic_coredump_available(void)
{
#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
    return s_have_summary;
#else
    return false;
#endif
}

bb_err_t bb_diag_panic_coredump_get(bb_diag_panic_summary_t *out)
{
    if (!out) return BB_ERR_INVALID_ARG;
#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
    if (!s_have_summary) return BB_ERR_NOT_FOUND;
    *out = s_summary;
    return BB_OK;
#else
    return BB_ERR_NOT_FOUND;
#endif
}

size_t bb_diag_panic_coredump_size(void)
{
#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
    if (!s_have_summary) return 0;
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK) return 0;
    return size;
#else
    return 0;
#endif
}

bb_err_t bb_diag_panic_coredump_read_bytes(uint8_t *buf, size_t max_len, size_t *out_len)
{
    if (!buf || max_len == 0 || !out_len) return BB_ERR_INVALID_ARG;
#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
    if (!s_have_summary) return BB_ERR_NOT_FOUND;

    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK) return BB_ERR_NOT_FOUND;

    *out_len = size;
    if (size > max_len) return BB_ERR_NO_SPACE;

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) return BB_ERR_NOT_FOUND;

    // esp_core_dump_image_get returns absolute flash addr; compute offset within partition:
    size_t part_offset = (addr >= part->address) ? (addr - part->address) : 0;
    if (esp_partition_read(part, part_offset, buf, size) != ESP_OK) {
        return BB_ERR_INVALID_STATE;
    }
    return BB_OK;
#else
    (void)buf; (void)max_len;
    *out_len = 0;
    return BB_ERR_NOT_FOUND;
#endif
}

bb_err_t bb_diag_panic_app_sha(char *out, size_t out_size)
{
    if (!out || out_size == 0) return BB_ERR_INVALID_ARG;
#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
    if (!s_have_summary || s_summary.app_sha256[0] == '\0') return BB_ERR_NOT_FOUND;
    strncpy(out, s_summary.app_sha256, out_size - 1);
    out[out_size - 1] = '\0';
    return BB_OK;
#else
    (void)out_size;
    return BB_ERR_NOT_FOUND;
#endif
}

void bb_diag_panic_coredump_erase(void)
{
#ifdef CONFIG_BB_DIAG_PANIC_COREDUMP
    if (!s_have_summary) return;
    esp_core_dump_image_erase();
    s_have_summary = false;
    s_summary.app_sha256[0] = '\0';
#endif
}

#else

// Stubs when panic capture is disabled

static bb_err_t bb_diag_panic_init(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    bool was_panic_boot = (reason == ESP_RST_PANIC || reason == ESP_RST_TASK_WDT ||
                           reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT ||
                           reason == ESP_RST_BROWNOUT);
    uint32_t stored_fp = 0;
    uint32_t stored_count = 0;
    bb_nv_get_u32(BB_DIAG_NV_NS, BB_DIAG_NV_KEY_APPFP, &stored_fp, 0);
    bb_nv_get_u32(BB_DIAG_NV_NS, BB_DIAG_NV_KEY_RST, &stored_count, 0);
    uint32_t running_fp = bb_diag_running_app_fp();
    bb_diag_reset_result_t r = bb_diag_reset_decision(stored_fp, running_fp,
                                                       stored_count, was_panic_boot);
    s_abnormal_reset_count = r.new_count;
    bb_nv_batch_t batch;
    if (bb_nv_batch_begin(&batch, BB_DIAG_NV_NS) == BB_OK) {
        bb_nv_batch_set_u32(&batch, BB_DIAG_NV_KEY_RST, r.new_count);
        if (r.store_fp) {
            bb_nv_batch_set_u32(&batch, BB_DIAG_NV_KEY_APPFP, running_fp);
        }
        bb_nv_batch_commit(&batch);
    }
    return BB_OK;
}

#include "bb_registry.h"
BB_REGISTRY_REGISTER_EARLY(bb_diag_panic, bb_diag_panic_init);

bool bb_diag_panic_available(void)
{
    return false;
}

bb_err_t bb_diag_panic_get(char *out, size_t *len_inout)
{
    (void)out;
    (void)len_inout;
    return BB_ERR_NOT_FOUND;
}

void bb_diag_panic_clear(void)
{
}

bool bb_diag_panic_coredump_available(void)
{
    return false;
}

bb_err_t bb_diag_panic_coredump_get(bb_diag_panic_summary_t *out)
{
    (void)out;
    return BB_ERR_NOT_FOUND;
}

uint32_t bb_diag_panic_boots_since(void)
{
    return 0;
}

size_t bb_diag_panic_coredump_size(void)
{
    return 0;
}

bb_err_t bb_diag_panic_coredump_read_bytes(uint8_t *buf, size_t max_len, size_t *out_len)
{
    (void)buf; (void)max_len;
    if (out_len) *out_len = 0;
    return BB_ERR_NOT_FOUND;
}

bb_err_t bb_diag_panic_app_sha(char *out, size_t out_size)
{
    if (!out || out_size == 0) return BB_ERR_INVALID_ARG;
    out[0] = '\0';
    return BB_ERR_NOT_FOUND;
}

void bb_diag_panic_coredump_erase(void) {}

#endif /* CONFIG_BB_DIAG_PANIC_CAPTURE */
