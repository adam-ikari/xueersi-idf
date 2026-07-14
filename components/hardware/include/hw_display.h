#pragma once

#include "esp_lcd_panel_io.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* ── LCD / SPI2 timing ─────────────────────────────────── */
#define LCD_HOST                    SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ          (60 * 1000 * 1000)
#define LCD_NATIVE_H_RES            128
#define LCD_NATIVE_V_RES            160
#define LCD_H_RES                   160
#define LCD_V_RES                   128
#define LCD_DRAW_BUF_LINES          LCD_V_RES
#define LCD_DRAW_BUF_COUNT          3
#define LCD_DPI                     60
#define LCD_CMD_BITS                8
#define LCD_PARAM_BITS              8

/* ── SPI2 pin assignments (TFT + shared SD) ────────────── */
#define PIN_NUM_LCD_SCLK            GPIO_NUM_18
#define PIN_NUM_LCD_MOSI            GPIO_NUM_23
#define PIN_NUM_LCD_MISO            GPIO_NUM_19
#define PIN_NUM_LCD_CS              GPIO_NUM_5
#define PIN_NUM_LCD_DC              GPIO_NUM_4
#define PIN_NUM_SD_CS               GPIO_NUM_22

/* ── Coordinate gaps ───────────────────────────────────── */
#define LCD_X_GAP                   0
#define LCD_Y_GAP                   0

/* ── ST7735 register set ───────────────────────────────── */
#define ST7735_SWRESET              0x01
#define ST7735_SLPOUT               0x11
#define ST7735_NORON                0x13
#define ST7735_INVOFF               0x20
#define ST7735_DISPOFF              0x28
#define ST7735_DISPON               0x29
#define ST7735_CASET                0x2A
#define ST7735_RASET                0x2B
#define ST7735_RAMWR                0x2C
#define ST7735_MADCTL               0x36
#define ST7735_COLMOD               0x3A
#define ST7735_FRMCTR1              0xB1
#define ST7735_FRMCTR2              0xB2
#define ST7735_FRMCTR3              0xB3
#define ST7735_INVCTR               0xB4
#define ST7735_PWCTR1               0xC0
#define ST7735_PWCTR2               0xC1
#define ST7735_PWCTR3               0xC2
#define ST7735_PWCTR4               0xC3
#define ST7735_PWCTR5               0xC4
#define ST7735_VMCTR1               0xC5
#define ST7735_GMCTRP1              0xE0
#define ST7735_GMCTRN1              0xE1

/* ── MADCTL bits ───────────────────────────────────────── */
#define MADCTL_MY                   0x80
#define MADCTL_MX                   0x40
#define MADCTL_MV                   0x20
#define MADCTL_RGB                  0x00

/* ── Public API ────────────────────────────────────────── */

/** One-shot init: SPI2 bus + panel-io + ST7735 init sequence.
 *  Returns ESP_OK on success. */
esp_err_t hw_display_init(void);

/** Return the panel-io handle created by hw_display_init(). */
esp_lcd_panel_io_handle_t hw_display_io(void);

/** Push a rectangle of pixel data to the ST7735 (CASET + RASET + RAMWR).
 *  px_map must point to at least (x2-x1+1)*(y2-y1+1)*2 bytes. */
void hw_display_flush(int x1, int y1, int x2, int y2, const uint8_t *px_map);

/** Send DISPON after the first flush (idempotent). */
void hw_display_on(void);

/** Has the first LVGL flush completed?  lcd_display_on() waits for this. */
bool hw_display_first_flush_done(void);

/** Trans-complete callback signature (matches esp_lcd_panel_io ISR cb). */
typedef bool (*hw_display_flush_ready_cb_t)(void *ctx);

/** Register the callback that is invoked from the trans-complete ISR.
 *  Pass NULL to clear.  Called once after every flush transaction. */
void hw_display_set_flush_ready_cb(hw_display_flush_ready_cb_t cb, void *ctx);
