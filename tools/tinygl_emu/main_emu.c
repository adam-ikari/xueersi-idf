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
#include "esp_compat.h"
#include "display_backend.h"
#include "GL/gl.h"
#include "zbuffer.h"

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
    (void)argc;
    (void)argv;

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
