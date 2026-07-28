/**
 * Display backend abstraction for TinyGL rendering engine.
 *
 * Different screen drivers (ST7735, ILI9341, GC9A01, etc.) implement this
 * interface. The engine only calls these functions — never touches hardware
 * registers directly.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    void   *(*init)(int width, int height, int pixel_format);
    void   (*clear)(uint16_t color);
    void   (*flush)(void);
    void  *(*get_buffer)(void);
    int    (*get_width)(void);
    int    (*get_height)(void);
} display_backend_t;

/* Pixel format IDs */
#define PIXEL_FORMAT_RGB565        0
#define PIXEL_FORMAT_RGB565_SWAP   1  /* byte-swapped for ST7735 */
#define PIXEL_FORMAT_RGBA8888      2