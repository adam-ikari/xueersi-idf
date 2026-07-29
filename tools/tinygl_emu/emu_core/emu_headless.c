/* emu_headless.c - Headless display backend: allocates a framebuffer, no SDL2 window */
#include "emu_headless.h"
#include <stdlib.h>
#include <string.h>

static uint16_t *s_fb = NULL;
static int s_w = 0, s_h = 0;

static void *headless_init(int w, int h, int fmt)
{
    (void)fmt;
    s_w = w; s_h = h;
    s_fb = (uint16_t *)malloc(w * h * 2);
    if (s_fb) memset(s_fb, 0, w * h * 2);
    return s_fb;
}

static void headless_clear(uint16_t c)
{
    if (s_fb) {
        for (int i = 0; i < s_w * s_h; i++) s_fb[i] = c;
    }
}

static void headless_flush(void) { /* no-op */ }

static void *headless_get_buffer(void) { return s_fb; }
static int   headless_get_width(void)  { return s_w; }
static int   headless_get_height(void) { return s_h; }

const display_backend_t emu_headless_backend = {
    .init       = headless_init,
    .clear      = headless_clear,
    .flush      = headless_flush,
    .get_buffer = headless_get_buffer,
    .get_width  = headless_get_width,
    .get_height = headless_get_height,
};