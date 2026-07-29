/**
 * TinyGL PC emulator entry point.
 *
 * Creates an SDL2 window, initializes TinyGL with the emulator display
 * backend, and runs a 60 Hz render loop. Press Ctrl+C to exit cleanly.
 *
 * Uses the shared render scene from main/tinygl_test.c, compiled with
 * -DTGL_EMU_BUILD to exclude ESP32-specific FreeRTOS task code.
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>

#define TGL_EMU_BUILD 1

#include "emu_display.h"
#include "emu_headless.h"
#include "emu_bmp.h"
#include "esp_compat.h"
#include "display_backend.h"
#include "GL/gl.h"
#include "zbuffer.h"

#include "test_rasterizer.h"

/* Symbols exported from main/tinygl_test.c (compiled with TGL_EMU_BUILD) */
extern int  gl_init(int w, int h);
extern void render_frame(float angle_y);
extern const display_backend_t *s_display;
extern volatile int tinygl_render_paused;
extern volatile int tinygl_log_enabled;
extern volatile float tinygl_last_fps;
extern volatile int tinygl_cube_count;
extern volatile int tinygl_physics_mode;

/* From main/tinygl_physics.c */
extern void physics_init(void);

static volatile int s_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    s_running = 0;
}

int main(int argc, char **argv)
{
    /* Check for test mode */
    if (argc > 1 && strcmp(argv[1], "--test-rasterizer") == 0) {
        test_result_t results[16];
        int total = test_rasterizer_run(results, 16);
        int passed = 0, failed = 0;
        printf("=== Rasterizer Unit Tests ===\n");
        for (int i = 0; i < total; i++) {
            printf("[%s] %s", results[i].passed ? "PASS" : "FAIL", results[i].name);
            if (!results[i].passed) {
                printf(" (diff_pixels=%d", results[i].diff_pixels);
                if (results[i].error_msg) printf(", error=%s", results[i].error_msg);
                printf(")");
                failed++;
            } else {
                passed++;
            }
            printf("\n");
        }
        printf("=== Results: %d passed, %d failed out of %d ===\n", passed, failed, total);
        return failed > 0 ? 1 : 0;
    }

    /* Offline render-to-BMP mode (headless, no SDL2 window) */
    if (argc > 1 && strcmp(argv[1], "--render-bmp") == 0) {
        const char *out_dir   = (argc > 2) ? argv[2] : ".";
        int num_frames        = (argc > 3) ? atoi(argv[3]) : 1;
        int cube_count        = (argc > 4) ? atoi(argv[4]) : 1;

        const display_backend_t *display = &emu_headless_backend;
        void *fb = display->init(160, 128, PIXEL_FORMAT_RGB565_SWAP);
        if (!fb) { fprintf(stderr, "Failed to init headless display\n"); return 1; }

        s_display = display;
        if (gl_init(160, 128) != 0) { fprintf(stderr, "gl_init failed\n"); return 1; }

        tinygl_cube_count = cube_count;
        tinygl_render_paused = 0;

        char path[256];
        for (int i = 0; i < num_frames; i++) {
            float angle = i * 10.0f;
            render_frame(angle);
            snprintf(path, sizeof(path), "%s/frame_%03d_c%d.bmp", out_dir, i, cube_count);
            if (bmp_save_rgb565((uint16_t *)fb, 160, 128, path) != 0) {
                fprintf(stderr, "Failed to save %s\n", path);
                return 1;
            }
            printf("Saved: %s\n", path);
        }
        return 0;
    }

    signal(SIGINT, signal_handler);

    /* Initialize emulator SDL2 display */
    const display_backend_t *display = &emu_display_backend;
    void *fb = display->init(160, 128, PIXEL_FORMAT_RGB565_SWAP);
    if (!fb) {
        fprintf(stderr, "Failed to initialize SDL2 display\n");
        return 1;
    }

    /* Bind the display backend (shared global in tinygl_test.c) */
    s_display = display;

    /* Initialize TinyGL + textures + lighting */
    if (gl_init(160, 128) != 0) {
        fprintf(stderr, "TinyGL gl_init failed\n");
        return 1;
    }

    /* Initialize physics engine */
    physics_init();

    printf("TinyGL PC emulator started. Press Ctrl+C to exit.\n");

    int frame_count = 0;
    float angle_y = 0;
    int64_t start_us = esp_timer_get_time();

    while (s_running) {
        int64_t frame_start = esp_timer_get_time();

        if (!tinygl_render_paused) {
            render_frame(angle_y);
            frame_count++;
            angle_y += 2.0f;
        }

        /* Cap at ~60 FPS */
        int64_t elapsed = esp_timer_get_time() - frame_start;
        if (elapsed < 16667) {
            usleep(16667 - (int)elapsed);
        }

        /* Report FPS every 5 seconds */
        int64_t total_elapsed = esp_timer_get_time() - start_us;
        if (total_elapsed >= 5000000) {
            float fps = (float)frame_count / ((float)total_elapsed / 1000000.0f);
            tinygl_last_fps = fps;
            if (tinygl_log_enabled) {
                printf("=== BENCHMARK === Frames: %d in %.2f sec = %.1f FPS\n",
                       frame_count, (float)total_elapsed / 1000000.0f, fps);
            }
            frame_count = 0;
            start_us = esp_timer_get_time();
        }
    }

    printf("Exiting...\n");
    return 0;
}
