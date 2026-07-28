/*
 * Canvas 2D Host API for WAMR
 *
 * Provides a complete Canvas 2D drawing API that WASM games can call.
 * The backend renders into the hardware framebuffer (hw_fb) on the
 * ST7735 160x128 RGB565 display.
 *
 * All functions return int32 (0 = success, -1 = error).
 * Registered as WAMR NativeSymbols under the "xiaomiao" module namespace.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "wasm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Canvas dimensions                                                    */
/* ------------------------------------------------------------------ */

#define CANVAS_WIDTH  160
#define CANVAS_HEIGHT 128

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define MAX_TRANSFORM_STACK 16
#define MAX_IMAGES 64
#define MAX_PATH_POINTS 256
#define MAX_FONT_NAME 64

/* ------------------------------------------------------------------ */
/* Image type                                                          */
/* ------------------------------------------------------------------ */

typedef struct ximg_t ximg_t;

struct ximg_t {
    int width;
    int height;
    uint8_t *pixels;    /* raw pixel data */
    uint8_t *palette;   /* 256-entry RGB332 palette (for Indexed8) */
    bool is_indexed;    /* true = Indexed8, false = RGB565 */
    int ref_count;
};

/* ------------------------------------------------------------------ */
/* Initialization / registration                                       */
/* ------------------------------------------------------------------ */

void canvas_api_init(uint16_t *fb, int width, int height);
bool canvas_api_register(void);

#ifdef __cplusplus
}
#endif
