#include "bb_display_spi_common.h"
#include "bb_log.h"

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "bb_display_spi";

/*
 * esp_lcd_panel_draw_bitmap() is asynchronous — it queues the SPI/DMA
 * transaction and returns before the transfer completes (panel IO is
 * created with trans_queue_depth=10, so up to 10 draw_bitmap calls can
 * have transactions in flight simultaneously). bb_display_blit_spi()
 * reuses a single shared s_bounce buffer across chunks/glyphs; refilling
 * it before the prior DMA transfer has drained corrupts the in-flight
 * transfer's source data. s_color_trans_done_sem is signaled from the
 * panel IO's on_color_trans_done callback (once per esp_lcd_panel_
 * draw_bitmap call — esp_lcd_panel_io_spi.c marks en_trans_done_cb only
 * on the last queued chunk of each tx_color) and taken after every
 * draw_bitmap call so s_bounce is never touched while a transfer against
 * it is still in flight.
 *
 * This MUST be a counting semaphore, not binary. bb_display_clear_spi()
 * issues one draw_bitmap per scanline (up to panel height, e.g. 240+
 * rows) before ever calling wait — each of those completions gives the
 * semaphore from the ISR. A binary semaphore can only hold one
 * outstanding give; the rest are silently dropped (xSemaphoreGiveFromISR
 * returns false and is a no-op) and a *single* wait_color_trans_done()
 * call after the loop is satisfied by whichever completion lands first —
 * not necessarily the last — while up to (h-1) other rows' transactions
 * are still physically in flight. When the very next operation (e.g. a
 * per-glyph bb_display_blit_spi call from bb_display_draw_text) refills
 * s_bounce, it races those still-draining clear-scanline transactions,
 * which pulls stale/glyph data onto the wire for the tail of the clear
 * and — because the binary semaphore's accounting is now out of sync —
 * lets a subsequent wait_color_trans_done() consume a leftover clear-row
 * give instead of the glyph's own completion, letting the *next* glyph's
 * blit start before *this* glyph's DMA has actually finished. This was
 * the root cause of both the garbled 8x16 title text and the residual/
 * misaligned hostname (bb_display_clear_spi() never reliably finishing
 * before the next draw). A counting semaphore removes the drop: every
 * give is retained and wait_color_trans_done() is called once per
 * draw_bitmap issued (bb_display_clear_spi loops it per scanline), so
 * gives and takes always balance 1:1 regardless of queue depth or burst
 * size. This assumes bb_display_blit_spi/bb_display_clear_spi are only
 * ever called from bb_display's single drawing task/context — the
 * semaphore here gates DMA completion, not cross-task synchronization. */
static SemaphoreHandle_t s_color_trans_done_sem = NULL;

/* Upper bound on outstanding (given-but-not-yet-taken) color-trans-done
 * completions. Sized to the largest single-call scanline burst
 * (bb_display_clear_spi issues one draw_bitmap per row, up to the tallest
 * supported panel height) with generous headroom. This is not a tight or
 * load-bearing bound — the largest realistic in-flight count is one
 * full-screen clear (panel height in rows, typically well under a few
 * hundred); 4096 is generous headroom above that, not a value that needs
 * to track any specific panel's height. */
#define BB_DISPLAY_SPI_MAX_INFLIGHT_GIVES 4096

static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                           esp_lcd_panel_io_event_data_t *edata,
                                           void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;
    BaseType_t woken = pdFALSE;
    if (s_color_trans_done_sem) {
        xSemaphoreGiveFromISR(s_color_trans_done_sem, &woken);
    }
    return woken == pdTRUE;
}

/* NOTE: this give/take accounting (and the counting semaphore above) is
 * ESP-IDF-only — it depends on the real esp_lcd_panel_io SPI transaction
 * queue and its on_color_trans_done callback firing from an ISR. There is
 * no host fake-panel seam to exercise this path off-device, so it is
 * excluded from host coverage gates and validated on hardware only. */

/* Block until the most recent draw_bitmap transfer has completed. Called
 * after every chunk so the bounce buffer is safe to refill/reuse. */
static void wait_color_trans_done(void)
{
    if (s_color_trans_done_sem) {
        xSemaphoreTake(s_color_trans_done_sem, portMAX_DELAY);
    }
}

/* Lazily create the completion semaphore the first time a panel IO is
 * created. Only one SPI display backend is ever active at a time (the
 * ordered probe-and-select-first-success walk in bb_display_init), so a
 * single shared semaphore — like the shared s_bounce buffer below — is
 * sufficient. */
static void ensure_color_trans_done_sem(void)
{
    if (!s_color_trans_done_sem) {
        s_color_trans_done_sem = xSemaphoreCreateCounting(BB_DISPLAY_SPI_MAX_INFLIGHT_GIVES, 0);
        if (!s_color_trans_done_sem) {
            bb_log_e(TAG, "color-trans-done sem alloc failed; SPI blit DMA-sync disabled (display may corrupt)");
        }
    }
}

bb_err_t bb_display_spi_init_bus_only(int pin_mosi, int pin_miso, int pin_clk,
                                       int max_transfer_sz, int host)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = pin_mosi,
        .miso_io_num     = pin_miso,
        .sclk_io_num     = pin_clk,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = max_transfer_sz,
    };
    esp_err_t err = spi_bus_initialize((spi_host_device_t)host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        bb_log_e(TAG, "spi bus init failed: %s", esp_err_to_name(err));
        return err;
    }
    return BB_OK;
}

bb_err_t bb_display_spi_new_panel_io(int host, int pclk_hz, int pin_cs, int pin_dc,
                                      esp_lcd_panel_io_handle_t *out_io)
{
    ensure_color_trans_done_sem();

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num          = pin_cs,
        .dc_gpio_num          = pin_dc,
        .spi_mode             = 0,
        .pclk_hz              = pclk_hz,
        .trans_queue_depth    = 10,
        .lcd_cmd_bits         = 8,
        .lcd_param_bits       = 8,
        .on_color_trans_done  = on_color_trans_done,
    };
    esp_err_t err = esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)(intptr_t)host, &io_cfg, out_io);
    if (err != ESP_OK) {
        bb_log_e(TAG, "panel io init failed: %s", esp_err_to_name(err));
        *out_io = NULL;
        return err;
    }
    return BB_OK;
}

/* Back-compat wrapper — existing callers (st77xx) keep building unchanged. */
bb_err_t bb_display_spi_init_bus(int pin_mosi, int pin_miso, int pin_clk,
                                  int max_transfer_sz, int host,
                                  int pclk_hz, int pin_cs, int pin_dc,
                                  esp_lcd_panel_io_handle_t *out_io)
{
    bb_err_t err = bb_display_spi_init_bus_only(
        pin_mosi, pin_miso, pin_clk, max_transfer_sz, host);
    if (err != BB_OK) return err;
    return bb_display_spi_new_panel_io(host, pclk_hz, pin_cs, pin_dc, out_io);
}

enum { BOUNCE_PIXELS = 512 };
static uint16_t s_bounce[BOUNCE_PIXELS];

void bb_display_blit_spi(esp_lcd_panel_handle_t panel,
                         int16_t x, int16_t y,
                         uint16_t w, uint16_t h,
                         const uint16_t *pixels)
{
    if (!panel || !pixels || !w || !h) return;

    int16_t row = 0;
    while (row < (int16_t)h) {
        size_t rows_this_pass = BOUNCE_PIXELS / w;
        if (rows_this_pass == 0) rows_this_pass = 1;
        if ((size_t)(h - row) < rows_this_pass) rows_this_pass = (size_t)(h - row);
        size_t pixels_this_pass = rows_this_pass * w;
        if (pixels_this_pass > BOUNCE_PIXELS) pixels_this_pass = BOUNCE_PIXELS;
        for (size_t i = 0; i < pixels_this_pass; i++) {
            uint16_t c = pixels[row * w + i];
            s_bounce[i] = (uint16_t)((c >> 8) | (c << 8));
        }
        esp_err_t err = esp_lcd_panel_draw_bitmap(panel, x, y + row, x + w, y + row + (int16_t)rows_this_pass, s_bounce);
        if (err != ESP_OK) {
            bb_log_w(TAG, "draw_bitmap failed: %s; skipping wait for this chunk", esp_err_to_name(err));
        } else {
            /* Wait for this chunk's DMA transfer to complete before the next
             * loop iteration (or the caller's next blit) refills s_bounce. */
            wait_color_trans_done();
        }
        row += (int16_t)rows_this_pass;
    }
}

void bb_display_clear_spi(esp_lcd_panel_handle_t panel,
                          uint16_t x, uint16_t y,
                          uint16_t w, uint16_t h,
                          uint16_t rgb565_swapped)
{
    if (!panel || !w || !h) return;

    /* Fill bounce buffer with the solid color once; reuse for every scanline. */
    size_t fill = w < BOUNCE_PIXELS ? w : BOUNCE_PIXELS;
    for (size_t i = 0; i < fill; i++) s_bounce[i] = rgb565_swapped;

    size_t issued = 0;
    for (uint16_t row = 0; row < h; row++) {
        esp_err_t err = esp_lcd_panel_draw_bitmap(panel, x, y + row, x + w, y + row + 1, s_bounce);
        if (err != ESP_OK) {
            bb_log_w(TAG, "draw_bitmap failed on row %u: %s; skipping its wait", (unsigned)row, esp_err_to_name(err));
        } else {
            issued++;
        }
    }
    /* All rows use identical bounce content, so it is safe to let them
     * pipeline through the DMA queue (trans_queue_depth) rather than
     * waiting per-row before queuing the next one. But each successfully
     * issued draw_bitmap call above still signals its own completion, so
     * the caller must consume exactly `issued` gives — a failed call
     * queues no transaction and fires no done-callback, so it must not be
     * waited on (see s_color_trans_done_sem's comment above). Waiting `h`
     * times unconditionally would deadlock forever if any row failed. */
    for (size_t i = 0; i < issued; i++) {
        wait_color_trans_done();
    }
}
