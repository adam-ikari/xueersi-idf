#include "hw_display.h"

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stddef.h>
#include <string.h>
#include <sys/param.h>

static const char *TAG = "hw_display";

/* ── Module state ──────────────────────────────────────── */
static esp_lcd_panel_io_handle_t s_lcd_io_handle;
static bool s_lcd_display_on;
static volatile bool s_lcd_first_flush_done;

static hw_display_flush_ready_cb_t s_flush_ready_cb;
static void *s_flush_ready_ctx;

/* ── Internal helpers (mirrors original st7735_*) ──────── */
static void st7735_tx_param(esp_lcd_panel_io_handle_t io_handle, int cmd,
                            const void *param, size_t param_size)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, cmd, param, param_size));
}

static void st7735_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void st7735_clear_black(esp_lcd_panel_io_handle_t io_handle)
{
    static uint16_t line[LCD_H_RES * 8];
    const uint8_t caset[] = {0x00, 0x00, 0x00, LCD_H_RES - 1};

    memset(line, 0, sizeof(line));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, ST7735_CASET, caset, sizeof(caset)));
    for (uint16_t y = 0; y < LCD_V_RES; y += 8) {
        const uint16_t y2 = MIN((uint16_t)(y + 7), (uint16_t)(LCD_V_RES - 1));
        const uint8_t raset[] = {
            y >> 8, y & 0xFF,
            y2 >> 8, y2 & 0xFF,
        };
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, ST7735_RASET, raset, sizeof(raset)));
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io_handle, ST7735_RAMWR, line,
                         (y2 - y + 1) * LCD_H_RES * sizeof(uint16_t)));
    }
}

static void st7735_init_black_tab_rot90(esp_lcd_panel_io_handle_t io_handle)
{
    const uint8_t frmctr[] = {0x01, 0x2C, 0x2D};
    const uint8_t frmctr3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    const uint8_t invctr[] = {0x07};
    const uint8_t pwctr1[] = {0xA2, 0x02, 0x84};
    const uint8_t pwctr2[] = {0xC5};
    const uint8_t pwctr3[] = {0x0A, 0x00};
    const uint8_t pwctr4[] = {0x8A, 0x2A};
    const uint8_t pwctr5[] = {0x8A, 0xEE};
    const uint8_t vmctr1[] = {0x0E};
    const uint8_t madctl_default[] = {MADCTL_MX | MADCTL_MY | MADCTL_RGB};
    const uint8_t colmod[] = {0x05};
    const uint8_t caset[] = {0x00, 0x00, 0x00, LCD_NATIVE_H_RES - 1};
    const uint8_t raset[] = {0x00, 0x00, 0x00, LCD_NATIVE_V_RES - 1};
    const uint8_t gamma_pos[] = {
        0x02, 0x1C, 0x07, 0x12,
        0x37, 0x32, 0x29, 0x2D,
        0x29, 0x25, 0x2B, 0x39,
        0x00, 0x01, 0x03, 0x10,
    };
    const uint8_t gamma_neg[] = {
        0x03, 0x1D, 0x07, 0x06,
        0x2E, 0x2C, 0x29, 0x2D,
        0x2E, 0x2E, 0x37, 0x3F,
        0x00, 0x00, 0x02, 0x10,
    };
    const uint8_t madctl_rot90[] = {MADCTL_MX | MADCTL_MV | MADCTL_RGB};

    ESP_LOGI(TAG, "Initialize ST7735R panel with MicroPython init(2) compatible sequence");
    st7735_tx_param(io_handle, ST7735_DISPOFF, NULL, 0);
    st7735_tx_param(io_handle, ST7735_SWRESET, NULL, 0);
    st7735_delay_ms(150);
    st7735_tx_param(io_handle, ST7735_SLPOUT, NULL, 0);
    st7735_delay_ms(500);
    st7735_tx_param(io_handle, ST7735_FRMCTR1, frmctr, sizeof(frmctr));
    st7735_tx_param(io_handle, ST7735_FRMCTR2, frmctr, sizeof(frmctr));
    st7735_tx_param(io_handle, ST7735_FRMCTR3, frmctr3, sizeof(frmctr3));
    st7735_tx_param(io_handle, ST7735_INVCTR, invctr, sizeof(invctr));
    st7735_tx_param(io_handle, ST7735_PWCTR1, pwctr1, sizeof(pwctr1));
    st7735_tx_param(io_handle, ST7735_PWCTR2, pwctr2, sizeof(pwctr2));
    st7735_tx_param(io_handle, ST7735_PWCTR3, pwctr3, sizeof(pwctr3));
    st7735_tx_param(io_handle, ST7735_PWCTR4, pwctr4, sizeof(pwctr4));
    st7735_tx_param(io_handle, ST7735_PWCTR5, pwctr5, sizeof(pwctr5));
    st7735_tx_param(io_handle, ST7735_VMCTR1, vmctr1, sizeof(vmctr1));
    st7735_tx_param(io_handle, ST7735_INVOFF, NULL, 0);
    st7735_tx_param(io_handle, ST7735_MADCTL, madctl_default, sizeof(madctl_default));
    st7735_tx_param(io_handle, ST7735_COLMOD, colmod, sizeof(colmod));
    st7735_tx_param(io_handle, ST7735_CASET, caset, sizeof(caset));
    st7735_tx_param(io_handle, ST7735_RASET, raset, sizeof(raset));
    st7735_tx_param(io_handle, ST7735_GMCTRP1, gamma_pos, sizeof(gamma_pos));
    st7735_tx_param(io_handle, ST7735_GMCTRN1, gamma_neg, sizeof(gamma_neg));
    st7735_tx_param(io_handle, ST7735_NORON, NULL, 0);
    st7735_delay_ms(10);
    st7735_tx_param(io_handle, ST7735_MADCTL, madctl_rot90, sizeof(madctl_rot90));
    st7735_clear_black(io_handle);
}

/* ── Internal ISR callback ─────────────────────────────── */
static bool display_flush_ready_isr(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;

    s_lcd_first_flush_done = true;
    if (s_flush_ready_cb) {
        s_flush_ready_cb(s_flush_ready_ctx);
    }
    return false;
}

/* ── Public API ────────────────────────────────────────── */

esp_err_t hw_display_init(void)
{
    ESP_LOGI(TAG, "Initialize SPI bus for ST7735 TFT");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_LCD_SCLK,
        .mosi_io_num = PIN_NUM_LCD_MOSI,
        .miso_io_num = PIN_NUM_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_BUF_LINES * sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_LCD_DC,
        .cs_gpio_num = PIN_NUM_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                   &io_config, &io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Panel IO create failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Register internal ISR callback so that s_lcd_first_flush_done
     * gets set and the user's callback is dispatched. */
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = display_flush_ready_isr,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, NULL));

    s_lcd_io_handle = io_handle;
    s_lcd_display_on = false;
    s_lcd_first_flush_done = false;
    st7735_init_black_tab_rot90(io_handle);

    return ESP_OK;
}

esp_lcd_panel_io_handle_t hw_display_io(void)
{
    return s_lcd_io_handle;
}

void hw_display_flush(int x1, int y1, int x2, int y2, const uint8_t *px_map)
{
    const int width = x2 - x1 + 1;
    const int height = y2 - y1 + 1;
    const uint16_t x_start = x1 + LCD_X_GAP;
    const uint16_t x_end = x2 + LCD_X_GAP;
    const uint16_t y_start = y1 + LCD_Y_GAP;
    const uint16_t y_end = y2 + LCD_Y_GAP;
    const uint8_t caset[] = {
        x_start >> 8, x_start & 0xFF,
        x_end >> 8, x_end & 0xFF,
    };
    const uint8_t raset[] = {
        y_start >> 8, y_start & 0xFF,
        y_end >> 8, y_end & 0xFF,
    };

    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_lcd_io_handle, ST7735_CASET, caset, sizeof(caset)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_lcd_io_handle, ST7735_RASET, raset, sizeof(raset)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(s_lcd_io_handle, ST7735_RAMWR,
                     px_map, width * height * sizeof(uint16_t)));
}

void hw_display_on(void)
{
    if (s_lcd_display_on || !s_lcd_io_handle) {
        return;
    }

    st7735_tx_param(s_lcd_io_handle, ST7735_DISPON, NULL, 0);
    st7735_delay_ms(20);
    s_lcd_display_on = true;
}

bool hw_display_first_flush_done(void)
{
    return s_lcd_first_flush_done;
}

void hw_display_set_flush_ready_cb(hw_display_flush_ready_cb_t cb, void *ctx)
{
    s_flush_ready_cb = cb;
    s_flush_ready_ctx = ctx;
}
