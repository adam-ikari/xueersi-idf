#include "test_frame.h"
#include "zbuffer.h"
#include "display_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_W 160
#define TEST_H 128

extern void render_frame(float angle_y);
extern int gl_init(int w, int h);
extern volatile int tinygl_render_paused;
extern volatile int tinygl_cube_count;
extern const display_backend_t *s_display;
extern ZBuffer *s_zb;

static uint16_t* dummy_fb = NULL;

static void* dummy_get_buffer(void) { return dummy_fb; }
static void dummy_flush(void) { /* no-op for headless testing */ }

static int load_ref_image(const char* path, uint16_t* buf, int w, int h) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 2, w * h, f);
    fclose(f);
    return (n == (size_t)(w * h)) ? 0 : -1;
}

static int save_image(const char* path, uint16_t* buf, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = fwrite(buf, 2, w * h, f);
    fclose(f);
    return (n == (size_t)(w * h)) ? 0 : -1;
}

static int compare_fb(uint16_t* a, uint16_t* b, int w, int h) {
    int diff = 0;
    for (int i = 0; i < w * h; i++) {
        uint16_t da = a[i] ^ b[i];
        int dr = (da >> 11) & 0x1F;
        int dg = (da >> 5) & 0x3F;
        int db = da & 0x1F;
        if (dr > 1 || dg > 1 || db > 1) diff++;
    }
    return diff;
}

static frame_test_result_t test_frame_cube1_angle0(void) {
    frame_test_result_t r = {"cube1_angle0", 0, 0, 0.0f, "emu_tests/test_ref/cube1_angle0.raw", NULL};

    /* Ensure TinyGL is initialized */
    if (!s_zb) {
        /* Use a dummy display backend that allocates a framebuffer */
        dummy_fb = (uint16_t*)malloc(TEST_W * TEST_H * 2);
        if (!dummy_fb) {
            r.error_msg = "Failed to allocate dummy framebuffer";
            return r;
        }
        memset(dummy_fb, 0, TEST_W * TEST_H * 2);
        static display_backend_t dummy_display = {
            .init = NULL,
            .clear = NULL,
            .flush = dummy_flush,
            .get_buffer = dummy_get_buffer,
            .get_width = NULL,
            .get_height = NULL,
        };
        s_display = &dummy_display;
        if (gl_init(TEST_W, TEST_H) != 0) {
            r.error_msg = "gl_init failed";
            return r;
        }
    }

    /* Set scene parameters */
    tinygl_cube_count = 1;
    tinygl_render_paused = 0;

    /* Render one frame */
    render_frame(0.0f);

    /* Get framebuffer */
    uint16_t* fb = (uint16_t*)s_zb->pbuf;

    /* Load reference image */
    uint16_t ref[TEST_W * TEST_H];
    if (load_ref_image(r.ref_path, ref, TEST_W, TEST_H) < 0) {
        /* Reference image does not exist — save current as reference */
        save_image(r.ref_path, fb, TEST_W, TEST_H);
        r.passed = 1;
        r.error_msg = "Reference image created (first run)";
        return r;
    }

    r.diff_pixels = compare_fb(fb, ref, TEST_W, TEST_H);
    r.diff_percent = (float)r.diff_pixels / (TEST_W * TEST_H) * 100.0f;
    r.passed = (r.diff_pixels < 50);  /* allow < 0.25 % difference */

    return r;
}

int test_frame_run(frame_test_result_t* results, int max_results) {
    int count = 0;
    if (count < max_results) results[count++] = test_frame_cube1_angle0();
    return count;
}
