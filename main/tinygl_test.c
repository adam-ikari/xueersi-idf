/**
 * TinyGL demo + benchmark on the Xiaomiao ESP32 handheld.
 *
 * This is the platform entry point. It initialises the ST7735 display,
 * the TinyGL rendering pipeline, and enters the render loop pinned to
 * core 1 (core 0 is free for the debug console, I2C, etc.).
 *
 * Rendering geometry and GL state live in tinygl_pipeline.{c,h}.
 * This file only manages the render loop, frame timing, and FPS
 * reporting.
 */

#include "tinygl_pipeline.h"
#include "display_backend.h"
#include "zgl.h"

#ifndef TGL_EMU_BUILD
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include "esp_compat.h"
#endif

#include <math.h>

static const char *TAG = "test";

/* Declared in tinygl_pipeline.c — we only read them. */
extern volatile int tinygl_log_enabled;
extern volatile float tinygl_last_fps;

/* ── Render task (pinned to core 1) ──────────────────── */
#ifndef TGL_EMU_BUILD
static void tinygl_render_task(void *arg)
{
    (void)arg;
    extern const display_backend_t st7735_display_backend;
    const display_backend_t *disp = &st7735_display_backend;

    int frame_count = 0;
    float angle_y   = 0.0f;
    int64_t start_us = esp_timer_get_time();
    int64_t frame_start_us;

    ESP_LOGI(TAG, "Render task started on core %d", xPortGetCoreID());

    while (1) {
        frame_start_us = esp_timer_get_time();

        if (!tinygl_render_paused) {
            /* Wait for previous frame's DMA to complete before writing
             * pbuf — prevents render/DMA race (tearing / missing polygons). */
            disp->wait_dma();

            render_frame(angle_y);
            frame_count++;
            angle_y += 2.0f;

            /* Print framebuffer diagnostics every 200 frames. */
            if ((frame_count % 200) == 0) {
                GLContext *c = gl_get_context();
                if (c && c->zb && c->zb->pbuf) {
                    int y = c->zb->ysize / 2;
                    uint16_t *fb = (uint16_t *)c->zb->pbuf;
                    ESP_LOGI("diag", "after_render: fb[0..7]=%04x %04x %04x %04x %04x %04x %04x %04x",
                             fb[y*160+0], fb[y*160+1], fb[y*160+2], fb[y*160+3],
                             fb[y*160+4], fb[y*160+5], fb[y*160+6], fb[y*160+7]);
                }
            }
        }

        /* Frame-rate limiter: target 30 FPS for stable, tear-free display. */
        int64_t frame_elapsed = esp_timer_get_time() - frame_start_us;
        int64_t target_frame_us = 33333;  /* 30 FPS = 33.3 ms */
        if (frame_elapsed < target_frame_us) {
            int64_t remain_us = target_frame_us - frame_elapsed;
            if (remain_us > 1000) {
                vTaskDelay(pdMS_TO_TICKS(remain_us / 1000));
            }
            /* Fine-spin remaining <1 ms for precision. */
            while (esp_timer_get_time() - frame_start_us < target_frame_us) {}
        } else {
            vTaskDelay(1);
        }

        /* FPS report every 5 seconds. */
        int64_t elapsed_us = esp_timer_get_time() - start_us;
        if (elapsed_us >= 5000000) {
            float fps = (float)frame_count / ((float)elapsed_us / 1000000.0f);
            if (tinygl_log_enabled) {
                ESP_LOGI(TAG, "Frames: %d in %.2f sec = %.1f FPS (core %d)",
                         frame_count, (float)elapsed_us / 1000000.0f, fps,
                         xPortGetCoreID());
            }
            tinygl_last_fps = fps;
            frame_count = 0;
            start_us = esp_timer_get_time();
        }
    }
}

/* ── Platform entry point ─────────────────────────────── */

void *tinygl_benchmark(void *arg)
{
    (void)arg;

    if (gl_init(160, 128) != 0) {
        ESP_LOGE(TAG, "gl_init failed");
        return NULL;
    }

    /* Pin the render loop to core 1 so core 0 stays free for debug
     * console, I2C, etc. */
    xTaskCreatePinnedToCore(
        tinygl_render_task,
        "tinygl_render",
        8192,               /* stack size */
        NULL,
        configMAX_PRIORITIES - 1,
        NULL,
        1                   /* core 1 */
    );

    /* Core 0: idle — available for debug console, I2C, etc. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return NULL;
}
#endif /* !TGL_EMU_BUILD */