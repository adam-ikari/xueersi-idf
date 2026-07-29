/**
 * ST7735 display backend implementation for the display_backend_t interface.
 *
 * 160x128 RGB565 byte-swapped, SPI2 DMA through hw_display_flush.
 * Dual framebuffers allocated in DMA-capable internal DRAM for async
 * transfer — render into one while DMA transmits the other.
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

/* Dual framebuffer: render to one while DMA transmits the other */
static uint16_t *s_fb[2]   = {NULL, NULL};
static int       s_fb_idx   = 0;  /* which buffer TinyGL renders into */
static int       s_w        = 0;
static int       s_h        = 0;
static int       s_fmt      = 0;

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

    s_fb[0] = (uint16_t *)heap_caps_malloc(w * h * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    s_fb[1] = (uint16_t *)heap_caps_malloc(w * h * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!s_fb[0] || !s_fb[1]) {
        ESP_LOGE(TAG, "Failed to allocate dual framebuffers");
        return NULL;
    }

    memset(s_fb[0], 0, w * h * 2);
    memset(s_fb[1], 0, w * h * 2);
    s_fb_idx = 0;

#ifndef TGL_EMU_BUILD
    s_dma_done_sem = xSemaphoreCreateBinary();
    if (!s_dma_done_sem) {
        ESP_LOGE(TAG, "Failed to create DMA semaphore");
        return NULL;
    }
    hw_display_set_flush_ready_cb((void*)flush_ready_cb, s_dma_done_sem);
#endif

    hw_display_on();
    ESP_LOGI(TAG, "ST7735 dual-fb backend: %dx%d fmt=%d, fb[0]=%p fb[1]=%p", w, h, pixel_format, s_fb[0], s_fb[1]);
    return s_fb[0];  /* start rendering into buffer 0 */
}

static void st7735_clear(uint16_t color)
{
    uint16_t *fb = s_fb[s_fb_idx];
    if (!fb) return;
    for (int i = 0; i < s_w * s_h; i++) {
        fb[i] = color;
    }
}

static void st7735_flush(void)
{
    uint16_t *fb = s_fb[s_fb_idx];
    if (!fb) return;

#ifndef TGL_EMU_BUILD
    /* Wait for previous DMA to complete */
    if (s_dma_busy) {
        xSemaphoreTake(s_dma_done_sem, portMAX_DELAY);
        s_dma_busy = false;
    }

    /* Start DMA of current render buffer */
    s_dma_busy = true;
    hw_display_flush(0, 0, s_w - 1, s_h - 1, (uint8_t *)fb);

    /* Swap to other buffer for next frame */
    s_fb_idx ^= 1;
#else
    /* PC emulator: synchronous flush */
    s_fb_idx ^= 1;
#endif
}

static void *st7735_get_buffer(void)  { return s_fb[s_fb_idx]; }
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
