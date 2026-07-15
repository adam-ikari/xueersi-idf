/**
 * @file esp_bsp_sdl_xiaomiao.c
 * @brief Xiaomiao handheld (ESP32-WROVER-B + ST7735 160x128) board adapter
 *
 * Uses the hardware-layer hw_display module rather than ESP-BSP.
 * Backlight is hardwired to GD32 — no ESP32 control.
 * No touch on this hardware.
 */

#include "esp_bsp_sdl.h"
#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "hw_display.h"

#define SDL_PIXELFORMAT_RGB565 0x15151002u

static const char *TAG = "esp_bsp_sdl_xiaomiao";
static bool s_display_inited = false;

static esp_err_t xiaomiao_init(esp_bsp_sdl_display_config_t *config,
                                esp_lcd_panel_handle_t *panel_handle,
                                esp_lcd_panel_io_handle_t *panel_io_handle)
{
    ESP_LOGI(TAG, "Initializing Xiaomiao handheld display via hw_display");

    if (!config || !panel_handle || !panel_io_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    // Fill display config: 160x128, RGB565 (byte-swapped handled by hw_display)
    config->width = 160;
    config->height = 128;
    config->pixel_format = SDL_PIXELFORMAT_RGB565;
    config->max_transfer_sz = (160 * 128) * sizeof(uint16_t);
    config->has_touch = false;

    // hw_display is already initialized by hw_board_init() before
    // SDL3 ever calls esp_bsp_sdl_init(). The panel io handle is already alive.
    esp_lcd_panel_io_handle_t io = hw_display_io();
    if (!io) {
        ESP_LOGE(TAG, "hw_display_io() returned NULL — was hw_board_init() called?");
        return ESP_ERR_INVALID_STATE;
    }

    *panel_handle = NULL;  // We don't use panel handle — hw_display owns all SPI
    *panel_io_handle = io;

    s_display_inited = true;
    ESP_LOGI(TAG, "Xiaomiao display ready: 160x128 RGB565");
    return ESP_OK;
}

static esp_err_t xiaomiao_backlight_on(void)
{
    // Backlight is hardwired to GD32, not controllable from ESP32.
    return ESP_OK;
}

static esp_err_t xiaomiao_backlight_off(void)
{
    // Backlight is hardwired to GD32, not controllable from ESP32.
    return ESP_OK;
}

static esp_err_t xiaomiao_display_on_off(bool enable)
{
    if (!s_display_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enable) {
        hw_display_on();
    }
    // hw_display has no "off" function — display is always-on after first flush
    return ESP_OK;
}

static esp_err_t xiaomiao_touch_init(void)
{
    ESP_LOGW(TAG, "No touch hardware on Xiaomiao handheld");
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t xiaomiao_touch_read(esp_bsp_sdl_touch_info_t *touch_info)
{
    (void)touch_info;
    return ESP_ERR_NOT_SUPPORTED;
}

static const char *xiaomiao_get_name(void)
{
    return "Xiaomiao Handheld (ESP32 + ST7735)";
}

static esp_err_t xiaomiao_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing Xiaomiao display adapter");
    s_display_inited = false;
    return ESP_OK;
}

const esp_bsp_sdl_board_interface_t esp_bsp_sdl_xiaomiao_interface = {
    .init = xiaomiao_init,
    .backlight_on = xiaomiao_backlight_on,
    .backlight_off = xiaomiao_backlight_off,
    .display_on_off = xiaomiao_display_on_off,
    .touch_init = xiaomiao_touch_init,
    .touch_read = xiaomiao_touch_read,
    .get_name = xiaomiao_get_name,
    .deinit = xiaomiao_deinit,
    .board_name = "Xiaomiao Handheld",
};