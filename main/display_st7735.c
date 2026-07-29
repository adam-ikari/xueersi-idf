/**
 * ST7735 display backend implementation for the display_backend_t interface.
 *
 * 160x128 RGB565 byte-swapped, SPI2 DMA through hw_display_flush.
 * Single framebuffer mode: render into buffer, wait for DMA complete, then
 * start next frame. Simple and tear-free at 30 FPS (23ms render+DMA < 33ms).
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

/* Single framebuffer with DMA sync */
static uint16_t *s_fb     = NULL;
static int       s_w      = 0;
static int       s_h      = 0;
static int       s_fmt    = 0;

#ifndef TGL_EMU_BUILD
static SemaphoreHandle_t s_dma_done_sem = NULL;
static volatile bool     s_dma_busy = false;

static bool IRAM_ATTR flush_ready_cb(void *ctx) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &xHigherPriorityTaskWoken);
    return xHigherPriorityTaskWoken == pdTRUE;
}
#endif

static void *st7735_init(int w, int h, int pixel_format)
{
    s_w   = w;
    s_h   = h;
    s_fmt = pixel_format;

    s_fb = (uint16_t *)heap_caps_malloc(w * h * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!s_fb) {
        ESP_LOGE(TAG, "Failed to allocate framebuffer");
        return NULL;
    }

    memset(s_fb, 0, w * h * 2);

#ifndef TGL_EMU_BUILD
    s_dma_done_sem = xSemaphoreCreateBinary();
    if (!s_dma_done_sem) {
        ESP_LOGE(TAG, "Failed to create DMA semaphore");
        return NULL;
    }
    hw_display_set_flush_ready_cb(flush_ready_cb, s_dma_done_sem);
#endif

    hw_display_on();
    ESP_LOGI(TAG, "ST7735 single-fb backend: %dx%d fmt=%d, fb=%p", w, h, pixel_format, s_fb);
    return s_fb;
}

static void st7735_clear(uint16_t color)
{
    if (!s_fb) return;
    for (int i = 0; i < s_w * s_h; i++) {
        s_fb[i] = color;
    }
}

static void st7735_flush(void)
{
    if (!s_fb) return;

#ifndef TGL_EMU_BUILD
    /* Wait for previous DMA to complete before starting new one */
    if (s_dma_busy) {
        xSemaphoreTake(s_dma_done_sem, portMAX_DELAY);
        s_dma_busy = false;
    }

    /* Start DMA */
    s_dma_busy = true;
    hw_display_flush(0, 0, s_w - 1, s_h - 1, (uint8_t *)s_fb);
#else
    /* PC emulator: synchronous flush (no DMA) */
#endif
}

static void *st7735_get_buffer(void)  { return s_fb; }
static int  st7735_get_width(void)   { return s_w; }
static int  st7735_get_height(void)  { return s_h; }

const display_backend_t st7735_display_backend = {
    .init       = st7735_init,
    .clear      = st7735_clear,
    .flush      = st7735_flush,
    .get_buffer = st7735_get_buffer,
    .get_width  = st7735_get_width,
    .get_height = st7735_get_height,
};
