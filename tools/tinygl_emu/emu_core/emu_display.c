#include "emu_display.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static SDL_Window* s_window = NULL;
static SDL_Renderer* s_renderer = NULL;
static SDL_Texture* s_texture = NULL;
static uint16_t* s_fb = NULL;
static int s_w = 0;
static int s_h = 0;

static void* emu_init(int w, int h, int pixel_format) {
    (void)pixel_format;
    s_w = w;
    s_h = h;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return NULL;
    }

    /* 4x scaled window for visibility */
    s_window = SDL_CreateWindow("TinyGL Emulator",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 w * 4, h * 4, SDL_WINDOW_SHOWN);
    if (!s_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return NULL;
    }

    s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_ACCELERATED);
    if (!s_renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return NULL;
    }

    s_texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_RGB565,
                                   SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!s_texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return NULL;
    }

    s_fb = (uint16_t*)malloc(w * h * 2);
    if (!s_fb) {
        fprintf(stderr, "Failed to allocate framebuffer\n");
        return NULL;
    }

    memset(s_fb, 0, w * h * 2);
    return s_fb;
}

static void emu_clear(uint16_t color) {
    if (!s_fb) return;
    for (int i = 0; i < s_w * s_h; i++) {
        s_fb[i] = color;
    }
}

static void emu_flush(void) {
    if (!s_fb || !s_texture) return;

    /*
     * ESP32 framebuffer is big-endian RGB565 (TGL_PIXEL_BYTE_SWAP=1).
     * SDL2's SDL_PIXELFORMAT_RGB565 is little-endian (R5G6B5, low byte first).
     * We must swap bytes for correct color display on PC.
     */
    static uint16_t* temp = NULL;
    if (!temp) {
        temp = (uint16_t*)malloc(s_w * s_h * 2);
    }

    for (int i = 0; i < s_w * s_h; i++) {
        temp[i] = ((s_fb[i] & 0xFF) << 8) | ((s_fb[i] >> 8) & 0xFF);
    }

    SDL_UpdateTexture(s_texture, NULL, temp, s_w * 2);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);

    /* Process window events (allow close) */
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            exit(0);
        }
    }
}

static void* emu_get_buffer(void) { return s_fb; }
static int emu_get_width(void) { return s_w; }
static int emu_get_height(void) { return s_h; }

const display_backend_t emu_display_backend = {
    .init       = emu_init,
    .clear      = emu_clear,
    .flush      = emu_flush,
    .get_buffer = emu_get_buffer,
    .get_width  = emu_get_width,
    .get_height = emu_get_height,
};
