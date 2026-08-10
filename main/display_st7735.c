/**
 * ST7735 display backend implementation for the display_backend_t interface.
 *
 * 160x128 RGB565 byte-swapped, SPI2 DMA through hw_display_flush.
 * N-framebuffer pipeline (DISPLAY_NUM_BUFFERS): render → post-process → DMA.
 * flush() DMA-sends the current render target and advances the rotation;
 * wait_dma() only blocks if the buffer about to become the render target still
 * has a pending DMA (never in steady state: DMA 5.5ms vs 33ms frame, N buffers).
 *
 * Timeline per frame:
 *   wait_dma()              — block only if NEXT render target's DMA in flight
 *   get_buffer()            — current render target (rotates per flush)
 *   zb_set_pbuf()           — TinyGL renders into that target
 *   gl_post_process()       — no-op hook between render and DMA
 *   flush()                 — DMA current target, advance rotation
 *   vTaskDelay              — yield, remaining time to 30fps target
 */

#include "display_backend.h"

#ifdef TGL_EMU_BUILD
/* PC emulator: stubs for hw_display functions — not used at runtime */
#include "esp_compat.h"
#include "esp_heap_caps.h"
static void hw_display_on(void) {}
static void hw_display_flush(int x1, int y1, int x2, int y2, const uint8_t *px_map) {
    (void)x1; (void)y1; (void)x2; (void)y2; (void)px_map;
}
static void hw_display_set_flush_ready_cb(void *cb, void *ctx) { (void)cb; (void)ctx; }
#else
#include "hw_display.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#include <string.h>

static const char *TAG = "st7735_be";

#ifndef DISPLAY_NUM_BUFFERS
#define DISPLAY_NUM_BUFFERS 2   /* render / DMA pipeline (2-fb paced) */
#endif

static uint16_t *s_fb[DISPLAY_NUM_BUFFERS] = { 0 };
static int       s_cur = 0;                 /* current render target */
static int       s_w = 0, s_h = 0, s_fmt = 0;

#ifndef TGL_EMU_BUILD
static SemaphoreHandle_t s_dma_done_sem = NULL;
static volatile bool     s_dma_busy[DISPLAY_NUM_BUFFERS] = { false };
static volatile int      s_pending_buf = -1;   /* which buffer's DMA is in flight */

static bool IRAM_ATTR flush_ready_cb(void *ctx) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_pending_buf >= 0) s_dma_busy[s_pending_buf] = false;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &xHigherPriorityTaskWoken);
    return xHigherPriorityTaskWoken == pdTRUE;
}
#endif

static void *st7735_init(int w, int h, int pixel_format)
{
    s_w = w; s_h = h; s_fmt = pixel_format;
    for (int i = 0; i < DISPLAY_NUM_BUFFERS; i++) {
        s_fb[i] = (uint16_t *)heap_caps_malloc(w * h * 2,
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        if (!s_fb[i]) {
            ESP_LOGE(TAG, "fb[%d] alloc failed", i);
            for (int j = 0; j < i; j++) {        /* free already-allocated bufs */
                heap_caps_free(s_fb[j]);
                s_fb[j] = NULL;
            }
            return NULL;
        }
        memset(s_fb[i], 0, w * h * 2);
    }
#ifndef TGL_EMU_BUILD
    s_dma_done_sem = xSemaphoreCreateBinary();
    if (!s_dma_done_sem) { ESP_LOGE(TAG, "DMA sem failed"); return NULL; }
    hw_display_set_flush_ready_cb(flush_ready_cb, s_dma_done_sem);
#endif
    hw_display_on();
    ESP_LOGI(TAG, "ST7735 %d-fb backend: %dx%d fmt=%d", DISPLAY_NUM_BUFFERS, w, h, pixel_format);
    return s_fb[0];
}

static void st7735_clear(uint16_t color)
{
    if (!s_fb[s_cur]) return;
    for (int i = 0; i < s_w * s_h; i++) {
        s_fb[s_cur][i] = color;
    }
}

/* Only block if the buffer we're about to render into still has a pending
 * DMA. Steady state (DMA 5.5ms vs 33ms frame, N buffers): never blocks. */
static void st7735_wait_dma(void)
{
#ifndef TGL_EMU_BUILD
    if (s_dma_busy[s_cur]) {
        xSemaphoreTake(s_dma_done_sem, portMAX_DELAY);   /* ISR cleared s_dma_busy */
    }
#endif
}

/* DMA current render target, then advance rotation. Returns immediately —
 * DMA runs in background; the next st7735_wait_dma() synchronizes. */
static void st7735_flush(void)
{
    if (!s_fb[s_cur]) return;

#ifndef TGL_EMU_BUILD
    s_dma_busy[s_cur] = true;
    s_pending_buf = s_cur;
    hw_display_flush(0, 0, s_w - 1, s_h - 1, (uint8_t *)s_fb[s_cur]);
    s_cur = (s_cur + 1) % DISPLAY_NUM_BUFFERS;
#endif
}

static void *st7735_get_buffer(void)  { return s_fb[s_cur]; }
static int  st7735_get_width(void)    { return s_w; }
static int  st7735_get_height(void)   { return s_h; }

const display_backend_t st7735_display_backend = {
    .init       = st7735_init,
    .clear      = st7735_clear,
    .flush      = st7735_flush,
    .wait_dma   = st7735_wait_dma,
    .get_buffer = st7735_get_buffer,
    .get_width  = st7735_get_width,
    .get_height = st7735_get_height,
};
