#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Framebuffer dimensions (ST7735 160x128 RGB565) */
#define FB_WIDTH  160
#define FB_HEIGHT 128

/**
 * @brief Initialize the framebuffer module.
 *
 * Allocates 160x128x2 = 40KB RGB565 framebuffer in PSRAM.
 * Initializes to black (all zeros).
 */
void hw_fb_init(void);

/**
 * @brief Clear the entire framebuffer with a solid color.
 * @param color RGB565 color value.
 */
void hw_fb_clear(uint16_t color);

/**
 * @brief Set a single pixel in the framebuffer.
 * @param x X coordinate (0..159)
 * @param y Y coordinate (0..127)
 * @param color RGB565 color value.
 */
void hw_fb_set_pixel(int x, int y, uint16_t color);

/**
 * @brief Get the color of a single pixel.
 * @param x X coordinate (0..159)
 * @param y Y coordinate (0..127)
 * @return RGB565 color value, or 0 if out of bounds.
 */
uint16_t hw_fb_get_pixel(int x, int y);

/**
 * @brief Fill a rectangle with a solid color.
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param w Width (clamped to framebuffer bounds)
 * @param h Height (clamped to framebuffer bounds)
 * @param color RGB565 color value.
 */
void hw_fb_fill_rect(int x, int y, int w, int h, uint16_t color);

/**
 * @brief Blit a raw RGB565 pixel buffer into the framebuffer.
 * @param x Destination top-left X coordinate
 * @param y Destination top-left Y coordinate
 * @param w Source buffer width
 * @param h Source buffer height
 * @param data Source RGB565 pixel data (row-major)
 */
void hw_fb_blit(int x, int y, int w, int h, const uint16_t *data);

/**
 * @brief Flush the entire framebuffer to the display.
 *
 * Calls hw_display_flush(0, 0, FB_WIDTH - 1, FB_HEIGHT - 1, buffer).
 */
void hw_fb_flush(void);

/**
 * @brief Get a pointer to the raw framebuffer buffer.
 * @return Pointer to the framebuffer, or NULL if not initialized.
 */
uint16_t *hw_fb_buffer(void);

#ifdef __cplusplus
}
#endif
