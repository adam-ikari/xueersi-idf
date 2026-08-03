/**
 * Display backend abstraction for TinyGL rendering engine.
 *
 * Different screen drivers (ST7735, ILI9341, GC9A01, etc.) implement this
 * interface. The engine only calls these functions — never touches hardware
 * registers directly.
 *
 * The backend runs an N-framebuffer rotation (render → post-process → DMA):
 *   get_buffer() returns the CURRENT render target; flush() DMA-sends it and
 *   advances the rotation, so the target returned by the next get_buffer() is
 *   a different buffer. wait_dma() blocks only until the buffer about to
 *   become the render target is free of a pending DMA (no-op in steady state).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    void   *(*init)(int width, int height, int pixel_format);
    void   (*clear)(uint16_t color);
    void   (*flush)(void);       /* DMA current render target, advance rotation */
    void   (*wait_dma)(void);    /* wait until NEXT render target's DMA done */
    void  *(*get_buffer)(void);  /* current render target (rotates per flush) */
    int    (*get_width)(void);
    int    (*get_height)(void);
} display_backend_t;

/* Pixel format IDs */
#define PIXEL_FORMAT_RGB565        0
#define PIXEL_FORMAT_RGB565_SWAP   1  /* byte-swapped for ST7735 */
#define PIXEL_FORMAT_RGBA8888      2