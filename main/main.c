/*
 * Xiaomiao ESP32-WROVER-B LVGL 9.5 hardware pager.
 *
 * Board resources from README.md:
 *   ST7735-compatible SPI TFT, MicroSD on shared SPI2, 6 active-low keys,
 *   GPIO14 passive buzzer, GPIO36/GPIO39 ADC sensors, I2C0 GD32/MPU6050,
 *   and GPIO25/26/32/33 extension IO.
 *
 * Hardware layer is delegated to components/hardware (hw_* modules).
 * This file contains only LVGL UI code and board-state-to-widget mapping.
 */

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "wasm_export.h"
#include "wasm_test.wasm.h"
#include "hw_fb.h"
#include "canvas_api.h"
#include "input_api.h"
#include "audio_api.h"

/* Hardware component includes */
#include "hw_board.h"
#include "hw_display.h"
#include "hw_input.h"
#include "hw_i2c.h"
#include "hw_gd32.h"
#include "hw_mpu.h"
#include "hw_adc.h"
#include "hw_buzzer.h"
#include "hw_extio.h"
#include "hw_sd.h"

#include "tinygl_test.h"
#include "tinygl_pipeline.h"
#include "render_api.h"
#include "render_queue.h"
#include "tinygl_physics.h"
#include "display_backend.h"

#include "debug_console.h"

#include <pthread.h>

#ifndef CONFIG_IDF_TARGET
#define CONFIG_IDF_TARGET "esp32"
#endif

#ifndef CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 240
#endif

#ifndef CONFIG_ESPTOOLPY_FLASHFREQ
#define CONFIG_ESPTOOLPY_FLASHFREQ "unknown"
#endif

#ifndef CONFIG_ESPTOOLPY_FLASHSIZE
#define CONFIG_ESPTOOLPY_FLASHSIZE "unknown"
#endif

#ifndef CONFIG_ESPTOOLPY_FLASHMODE
#define CONFIG_ESPTOOLPY_FLASHMODE "unknown"
#endif

#if CONFIG_ESPTOOLPY_FLASHMODE_QIO
#define UI_FLASH_MODE "QIO"
#elif CONFIG_ESPTOOLPY_FLASHMODE_QOUT
#define UI_FLASH_MODE "QOUT"
#elif CONFIG_ESPTOOLPY_FLASHMODE_DIO
#define UI_FLASH_MODE "DIO"
#elif CONFIG_ESPTOOLPY_FLASHMODE_DOUT
#define UI_FLASH_MODE "DOUT"
#else
#define UI_FLASH_MODE CONFIG_ESPTOOLPY_FLASHMODE
#endif

#if CONFIG_ESPTOOLPY_FLASHFREQ_80M
#define UI_FLASH_FREQ "80 MHz"
#elif CONFIG_ESPTOOLPY_FLASHFREQ_40M
#define UI_FLASH_FREQ "40 MHz"
#elif CONFIG_ESPTOOLPY_FLASHFREQ_26M
#define UI_FLASH_FREQ "26 MHz"
#elif CONFIG_ESPTOOLPY_FLASHFREQ_20M
#define UI_FLASH_FREQ "20 MHz"
#else
#define UI_FLASH_FREQ CONFIG_ESPTOOLPY_FLASHFREQ
#endif

#if CONFIG_ESPTOOLPY_FLASHSIZE_4MB
#define UI_FLASH_SIZE "4 MB"
#elif CONFIG_ESPTOOLPY_FLASHSIZE_2MB
#define UI_FLASH_SIZE "2 MB"
#elif CONFIG_ESPTOOLPY_FLASHSIZE_8MB
#define UI_FLASH_SIZE "8 MB"
#elif CONFIG_ESPTOOLPY_FLASHSIZE_16MB
#define UI_FLASH_SIZE "16 MB"
#else
#define UI_FLASH_SIZE CONFIG_ESPTOOLPY_FLASHSIZE
#endif

#if CONFIG_IDF_TARGET_ESP32
#define UI_TARGET_NAME "ESP32"
#else
#define UI_TARGET_NAME CONFIG_IDF_TARGET
#endif

#ifndef CONFIG_SPIRAM_SPEED
#define CONFIG_SPIRAM_SPEED 0
#endif

/* LVGL / UI tuning constants (not hardware) */
#define LVGL_TICK_PERIOD_MS         1
#define LVGL_TASK_STACK_SIZE        (10 * 1024)
#define LVGL_TASK_PRIORITY          5
#define LVGL_TASK_MIN_DELAY_MS      1
#define LVGL_TASK_MAX_DELAY_MS      16

#define UI_REFRESH_PERIOD_MS        16
#define UI_ACTION_MSG_MS            850
#define UI_HISTORY_POINTS           48
#define LIGHT_HISTORY_AVG_SAMPLES   4
#define THERM_UPDATE_PERIOD_MS      1000
#define THERM_HISTORY_MIN_PCT       35
#define THERM_HISTORY_MAX_PCT       65

/* Page enum (only for LVGL build — SDL build gets it from sdl_demo.h) */
#if !CONFIG_XIAOMIAO_USE_SDL
typedef enum {
    UI_PAGE_LIGHT = 0,
    UI_PAGE_THERM,
    UI_PAGE_MOTION,
    UI_PAGE_LED1,
    UI_PAGE_LED2,
    UI_PAGE_BUZZER,
    UI_PAGE_MOTOR1,
    UI_PAGE_MOTOR2,
    UI_PAGE_SD,
    UI_PAGE_GPIO25,
    UI_PAGE_GPIO26,
    UI_PAGE_ADC32,
    UI_PAGE_ADC33,
    UI_PAGE_SYSTEM,
    UI_PAGE_ABOUT,
    UI_PAGE_COUNT,
} ui_page_t;

/* UI widget handle struct */
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *page;
    lv_obj_t *title;
    lv_obj_t *value;
    lv_obj_t *sub;
    lv_obj_t *bar;
    lv_obj_t *chart;
    lv_obj_t *chart_head;
    lv_obj_t *status;
    lv_obj_t *hint;
    lv_obj_t *accent;
    lv_chart_series_t *chart_series;
    uint32_t chart_history_version;
    lv_group_t *group;
    ui_page_t page_id;
} ui_state_t;
#endif /* !CONFIG_XIAOMIAO_USE_SDL */

static const char *TAG = "xiaomiao_dash";

#if !CONFIG_XIAOMIAO_USE_SDL
static lv_draw_buf_t s_draw_buf3;
static ui_state_t s_ui;

/* UI-level history buffers (not hardware) */
static int32_t s_light_history[UI_HISTORY_POINTS];
static int32_t s_therm_history[UI_HISTORY_POINTS];
static uint32_t s_light_history_version;
static uint32_t s_therm_history_version;
static uint32_t s_light_hist_accum;
static uint8_t s_light_hist_count;
static uint32_t s_therm_accum;
static uint16_t s_therm_accum_count;
static uint32_t s_last_therm_publish_ms;

/* UI-level state (not hardware) */
static uint32_t s_buzzer_freq_hz = 988;
static uint32_t s_action_until_ms;

/* Helpers */

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static void set_action(const char *msg)
{
    hw_board_state_t *board = hw_board_state_mut();
    copy_text(board->action, sizeof(board->action), msg);
    s_action_until_ms = lv_tick_get() + UI_ACTION_MSG_MS;
}

static void sensor_history_init(void)
{
    for (size_t i = 0; i < UI_HISTORY_POINTS; ++i) {
        s_light_history[i] = LV_CHART_POINT_NONE;
        s_therm_history[i] = LV_CHART_POINT_NONE;
    }
    s_light_history_version = 0;
    s_therm_history_version = 0;
    s_light_hist_accum = 0;
    s_light_hist_count = 0;
    s_therm_accum = 0;
    s_therm_accum_count = 0;
    s_last_therm_publish_ms = 0;
}

static void sensor_history_push(int32_t *history, uint32_t *version, int value)
{
    for (size_t i = 1; i < UI_HISTORY_POINTS; ++i) {
        history[i - 1] = history[i];
    }
    history[UI_HISTORY_POINTS - 1] = MAX(0, MIN(value, 100));
    (*version)++;
}

static void sensor_history_push_range(int32_t *history, uint32_t *version, int value, int min_value, int max_value)
{
    sensor_history_push(history, version, MAX(min_value, MIN(value, max_value)));
}

/* Accumulate ADC readings into UI history */
static void accumulate_adc_to_history(void)
{
    const hw_board_state_t *board = hw_board_state();
    s_light_hist_accum += pct_from_raw(board->light_raw);
    s_light_hist_count++;
    if (s_light_hist_count >= LIGHT_HISTORY_AVG_SAMPLES) {
        sensor_history_push(s_light_history,
                            &s_light_history_version,
                            (int)((s_light_hist_accum + s_light_hist_count / 2) / s_light_hist_count));
        s_light_hist_accum = 0;
        s_light_hist_count = 0;
    }

    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_therm_accum += board->temp_raw;
    s_therm_accum_count++;
    if (s_last_therm_publish_ms == 0 || now_ms - s_last_therm_publish_ms >= THERM_UPDATE_PERIOD_MS) {
        const int avg_raw = (int)((s_therm_accum + s_therm_accum_count / 2) / s_therm_accum_count);
        /* Write averaged temp_raw into board state for UI display */
        hw_board_state_t *b = hw_board_state_mut();
        b->temp_raw = avg_raw;
        sensor_history_push_range(s_therm_history,
                                  &s_therm_history_version,
                                  pct_from_raw(avg_raw),
                                  THERM_HISTORY_MIN_PCT,
                                  THERM_HISTORY_MAX_PCT);
        s_therm_accum = 0;
        s_therm_accum_count = 0;
        s_last_therm_publish_ms = now_ms;
    }
}

/* LVGL flush callback */
static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    (void)display;
    hw_display_flush(area->x1, area->y1, area->x2, area->y2, px_map);
}

/* LVGL tick callback */
static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

/* Bridge: flush-ready ISR from hw_display -> lv_display_flush_ready */
static bool lvgl_flush_ready_bridge(void *ctx)
{
    lv_display_t *display = (lv_display_t *)ctx;
    lv_display_flush_ready(display);
    return false;
}

/* Keypad input (LVGL-specific) */
static void keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static uint32_t last_key = LV_KEY_ENTER;
    int found_idx = -1;

    size_t count = 0;
    const hw_button_t *buttons = hw_input_buttons(&count);

    for (size_t i = 0; i < count; ++i) {
        if (hw_input_is_pressed(i)) {
            found_idx = (int)i;
            break;
        }
    }

    if (found_idx >= 0) {
        last_key = buttons[found_idx].key;
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = last_key;
    }
    else {
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = last_key;
    }
}

/* LVGL display init */
static lv_display_t *lvgl_display_init(void)
{
#if LCD_DRAW_BUF_LINES != LCD_V_RES
#error "Triple/full refresh mode requires LCD_DRAW_BUF_LINES to equal LCD_V_RES"
#endif
#if LCD_DRAW_BUF_COUNT != 3
#error "This build is configured for full-screen triple buffering"
#endif

    lv_display_t *display = lv_display_create(LCD_H_RES, LCD_V_RES);
    assert(display);

    const lv_color_format_t color_format = LV_COLOR_FORMAT_RGB565_SWAPPED;
    const uint32_t stride = lv_draw_buf_width_to_stride(LCD_H_RES, color_format);
    const size_t draw_buffer_sz = stride * LCD_DRAW_BUF_LINES;
    void *buf1 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    void *buf2 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    void *buf3 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    assert(buf1);
    assert(buf2);
    assert(buf3);

    lv_display_set_color_format(display, color_format);
    lv_display_set_dpi(display, LCD_DPI);
    lv_display_set_buffers(display, buf1, buf2, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_FULL);
    lv_result_t res = lv_draw_buf_init(&s_draw_buf3,
                                       LCD_H_RES,
                                       LCD_DRAW_BUF_LINES,
                                       color_format,
                                       stride,
                                       buf3,
                                       draw_buffer_sz);
    assert(res == LV_RESULT_OK);
    lv_display_set_3rd_draw_buffer(display, &s_draw_buf3);
    lv_display_set_user_data(display, hw_display_io());
    lv_display_set_flush_cb(display, lvgl_flush_cb);

    ESP_LOGI(TAG,
             "LVGL display: %dx%d, dpi=%d, %d full-screen DMA buffers, SPI=%d MHz",
             LCD_H_RES,
             LCD_V_RES,
             LCD_DPI,
             LCD_DRAW_BUF_COUNT,
             LCD_PIXEL_CLOCK_HZ / 1000000);

    return display;
}

/* LVGL input init */
static lv_group_t *lvgl_input_init(lv_display_t *display)
{
    lv_group_t *group = lv_group_create();
    assert(group);
    lv_group_set_default(group);

    lv_indev_t *indev = lv_indev_create();
    assert(indev);
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_display(indev, display);
    lv_indev_set_group(indev, group);
    lv_indev_set_read_cb(indev, keypad_read_cb);
    lv_indev_set_long_press_time(indev, 360);
    lv_indev_set_long_press_repeat_time(indev, 130);

    return group;
}

/* Page names */
static const char *const s_page_names[UI_PAGE_COUNT] = {
    "LIGHT",
    "THERM",
    "MOTION",
    "LED 1",
    "LED 2",
    "BUZZER",
    "MOTOR 1",
    "MOTOR 2",
    "SD CARD",
    "GPIO25",
    "GPIO26",
    "GPIO32",
    "GPIO33",
    "SYSTEM",
    "ABOUT",
};

/* UI colour constants */
static const uint32_t UI_YELLOW = 0xF6D34A;
static const uint32_t UI_BLACK = 0x1B1713;
static const uint32_t UI_BROWN = 0x5C4220;
static const uint32_t UI_RED = 0xE64B3C;
static const uint32_t UI_CREAM = 0xFFF3B0;
static const int UI_HISTORY_CHART_PAD_X = 2;
static const int UI_HISTORY_CHART_PAD_Y = 3;
static const int UI_HISTORY_HEAD_SIZE = 7;

/* Forward declarations */
static void ui_refresh(void);
static void ui_show_page(ui_page_t page, int dir);

/* Error string helper */
static const char *short_err(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return "OK";
    case ESP_ERR_TIMEOUT:
        return "TIMEOUT";
    case ESP_ERR_NOT_FOUND:
        return "NOT FOUND";
    case ESP_ERR_INVALID_STATE:
        return "STATE";
    case ESP_ERR_INVALID_ARG:
        return "ARG";
    case ESP_FAIL:
        return "FAIL";
    default:
        return "ERR";
    }
}

/* UI widget builders */
static lv_obj_t *ui_label(lv_obj_t *parent,
                          const char *text,
                          int y,
                          uint32_t color,
                          const lv_font_t *font,
                          lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(label, LCD_H_RES - 16, LV_SIZE_CONTENT);
    lv_obj_set_pos(label, 8, y);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

static lv_obj_t *ui_make_page(int x)
{
    lv_obj_t *page = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(page);
    lv_obj_set_pos(page, x, 0);
    lv_obj_set_size(page, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(page, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    if (s_ui.page_id == UI_PAGE_ABOUT) {
        lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(page, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_width(page, 3, LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_color(page, lv_color_hex(UI_BROWN), LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_opa(page, LV_OPA_80, LV_PART_SCROLLBAR);
    }
    else {
        lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    }
    return page;
}

static lv_obj_t *ui_bar(lv_obj_t *parent, int value)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, 18, 86);
    lv_obj_set_size(bar, 124, 8);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, value, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_CREAM), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_BLACK), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    return bar;
}

static bool ui_page_has_history(ui_page_t page)
{
    return page == UI_PAGE_LIGHT || page == UI_PAGE_THERM;
}

static int32_t *ui_history_for_page(ui_page_t page)
{
    return page == UI_PAGE_THERM ? s_therm_history : s_light_history;
}

static uint32_t ui_history_version_for_page(ui_page_t page)
{
    return page == UI_PAGE_THERM ? s_therm_history_version : s_light_history_version;
}

static uint32_t ui_history_color_for_page(ui_page_t page)
{
    (void)page;
    return UI_BROWN;
}

static int ui_history_min_for_page(ui_page_t page)
{
    return page == UI_PAGE_THERM ? THERM_HISTORY_MIN_PCT : 0;
}

static int ui_history_max_for_page(ui_page_t page)
{
    return page == UI_PAGE_THERM ? THERM_HISTORY_MAX_PCT : 100;
}

static lv_obj_t *ui_history_chart(lv_obj_t *parent, int32_t *history, uint32_t color)
{
    lv_obj_t *chart = lv_chart_create(parent);
    lv_obj_set_pos(chart, 18, 77);
    lv_obj_set_size(chart, 124, 25);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(chart, 0, 0);
    lv_obj_set_style_pad_left(chart, UI_HISTORY_CHART_PAD_X, 0);
    lv_obj_set_style_pad_right(chart, UI_HISTORY_CHART_PAD_X, 0);
    lv_obj_set_style_pad_top(chart, UI_HISTORY_CHART_PAD_Y, 0);
    lv_obj_set_style_pad_bottom(chart, UI_HISTORY_CHART_PAD_Y, 0);
    lv_obj_set_style_radius(chart, 4, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_color(chart, lv_color_hex(UI_BROWN), LV_PART_MAIN);
    lv_obj_set_style_line_opa(chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, UI_HISTORY_POINTS);
    lv_chart_set_axis_range(chart,
                            LV_CHART_AXIS_PRIMARY_Y,
                            ui_history_min_for_page(s_ui.page_id),
                            ui_history_max_for_page(s_ui.page_id));
    lv_chart_set_div_line_count(chart, 2, 4);

    s_ui.chart_series = lv_chart_add_series(chart, lv_color_hex(color), LV_CHART_AXIS_PRIMARY_Y);
    if (s_ui.chart_series) {
        lv_chart_set_series_ext_y_array(chart, s_ui.chart_series, history);
    }
    lv_chart_refresh(chart);
    return chart;
}

static lv_obj_t *ui_history_head_dot(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, UI_HISTORY_HEAD_SIZE, UI_HISTORY_HEAD_SIZE);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    return dot;
}

static void ui_update_history_head(void)
{
    if (!s_ui.chart || !s_ui.chart_series || !s_ui.chart_head) {
        return;
    }

    const int32_t *history = ui_history_for_page(s_ui.page_id);
    if (history[UI_HISTORY_POINTS - 1] == LV_CHART_POINT_NONE) {
        lv_obj_add_flag(s_ui.chart_head, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_point_t p;
    lv_chart_get_point_pos_by_id(s_ui.chart, s_ui.chart_series, UI_HISTORY_POINTS - 1, &p);
    const int32_t dot_half = UI_HISTORY_HEAD_SIZE / 2;
    lv_obj_set_pos(s_ui.chart_head,
                   lv_obj_get_x(s_ui.chart) + p.x - dot_half,
                   lv_obj_get_y(s_ui.chart) + p.y - dot_half);
    lv_obj_clear_flag(s_ui.chart_head, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.chart_head);
}

static void ui_set_bar(int value)
{
    if (s_ui.bar) {
        lv_bar_set_value(s_ui.bar, MAX(0, MIN(value, 100)), LV_ANIM_ON);
    }
}

static void ui_refresh_history_chart(void)
{
    const uint32_t version = ui_history_version_for_page(s_ui.page_id);
    if (s_ui.chart && s_ui.chart_history_version != version) {
        lv_chart_refresh(s_ui.chart);
        ui_update_history_head();
        s_ui.chart_history_version = version;
    }
}

static void ui_set_hint(const char *normal)
{
    if (!s_ui.hint) {
        return;
    }
    const hw_board_state_t *board = hw_board_state();
    if (s_action_until_ms && (int32_t)(s_action_until_ms - lv_tick_get()) > 0) {
        lv_label_set_text(s_ui.hint, board->action);
    }
    else {
        s_action_until_ms = 0;
        lv_label_set_text(s_ui.hint, normal);
    }
}

static unsigned ui_kb(size_t bytes)
{
    return (unsigned)((bytes + 512) / 1024);
}

/* About page */
static void ui_build_about_page(lv_obj_t *page)
{
    esp_chip_info_t chip_info;
    char details[1200];

    esp_chip_info(&chip_info);

    const size_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    snprintf(details,
             sizeof(details),
             "Model\n"
             "  Xiaomiao Handheld\n"
             "  ESP32-WROVER-B\n"
             "  Author: ZYoung\n\n"
             "CPU\n"
             "  Xtensa LX6\n"
             "    %d MHz x%u\n"
             "  Chip rev: %u\n\n"
             "System\n"
             "  ESP-IDF: %s\n"
             "  FreeRTOS: %s\n"
             "  Target: %s\n"
             "  Build: %s\n\n"
             "Clocks\n"
             "  Flash: %s %s\n"
             "  PSRAM: %d MHz\n"
             "  LCD SPI2: %u MHz\n"
             "  SD SPI2: %u MHz\n"
             "  I2C0: %u kHz\n"
             "  Light ADC: 60 Hz\n\n"
             "Storage\n"
             "  Flash: %s\n"
             "  SRAM: %u KB\n"
             "  PSRAM: %u KB\n\n"
             "Display\n"
             "  ST7735 160x128\n"
             "  SPI2 %u MHz\n"
             "  RGB565 DMA x%u\n"
             "  LVGL %d.%d.%d\n\n"
             "Board IO\n"
             "  Keys: 6 active-low\n"
             "  SD: SPI2 CS22\n"
             "  ADC: 36/39/32/33\n"
             "  I2C: GD32 0x40\n"
             "       MPU6050 0x68\n"
             "  PWM: GPIO14 Buzzer\n"
             "       GPIO25/26 EXT\n\n"
             "wechat/tel:\n"
             "  15657325738\n",
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
             (unsigned)chip_info.cores,
             (unsigned)chip_info.revision,
             esp_get_idf_version(),
             tskKERNEL_VERSION_NUMBER,
             UI_TARGET_NAME,
             __DATE__,
             UI_FLASH_MODE,
             UI_FLASH_FREQ,
             CONFIG_SPIRAM_SPEED,
             (unsigned)(LCD_PIXEL_CLOCK_HZ / 1000000),
             (unsigned)(SD_SPI_MAX_FREQ_KHZ / 1000),
             (unsigned)(I2C_FREQ_HZ / 1000),
             UI_FLASH_SIZE,
             ui_kb(sram_total),
             ui_kb(psram_total),
             (unsigned)(LCD_PIXEL_CLOCK_HZ / 1000000),
             (unsigned)LCD_DRAW_BUF_COUNT,
             LVGL_VERSION_MAJOR,
             LVGL_VERSION_MINOR,
             LVGL_VERSION_PATCH);

    lv_obj_t *label = lv_label_create(page);
    lv_label_set_text(label, details);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(label, 8, 28);
    lv_obj_set_width(label, LCD_H_RES - 22);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_line_space(label, 1, 0);

    s_ui.value = lv_label_create(page);
    lv_obj_add_flag(s_ui.value, LV_OBJ_FLAG_HIDDEN);
    s_ui.sub = lv_label_create(page);
    lv_obj_add_flag(s_ui.sub, LV_OBJ_FLAG_HIDDEN);
    s_ui.hint = lv_label_create(page);
    lv_obj_add_flag(s_ui.hint, LV_OBJ_FLAG_HIDDEN);
    s_ui.bar = NULL;
    s_ui.chart = NULL;
    s_ui.chart_head = NULL;
    s_ui.chart_series = NULL;
}

/* Build page content */
static void ui_build_page_content(lv_obj_t *page)
{
    char idx[10];

    s_ui.title = ui_label(page, s_page_names[s_ui.page_id], 7, UI_BLACK, &lv_font_montserrat_14, LV_TEXT_ALIGN_LEFT);
    snprintf(idx, sizeof(idx), "%02u/%02u", (unsigned)s_ui.page_id + 1, (unsigned)UI_PAGE_COUNT);
    s_ui.status = ui_label(page, idx, 7, UI_BROWN, &lv_font_montserrat_10, LV_TEXT_ALIGN_RIGHT);

    s_ui.accent = lv_obj_create(page);
    lv_obj_remove_style_all(s_ui.accent);
    lv_obj_set_size(s_ui.accent, 13, 13);
    lv_obj_set_pos(s_ui.accent, 132, 25);
    lv_obj_set_style_radius(s_ui.accent, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ui.accent, lv_color_hex(UI_RED), 0);
    lv_obj_set_style_bg_opa(s_ui.accent, LV_OPA_COVER, 0);

    if (s_ui.page_id == UI_PAGE_ABOUT) {
        ui_build_about_page(page);
        return;
    }

    s_ui.value = ui_label(page, "--", 38, UI_BLACK, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);
    s_ui.sub = ui_label(page, "--", 63, UI_BROWN, &lv_font_montserrat_10, LV_TEXT_ALIGN_CENTER);
    if (ui_page_has_history(s_ui.page_id)) {
        const uint32_t color = ui_history_color_for_page(s_ui.page_id);
        s_ui.bar = NULL;
        s_ui.chart = ui_history_chart(page,
                                      ui_history_for_page(s_ui.page_id),
                                      color);
        s_ui.chart_head = ui_history_head_dot(page, color);
    }
    else {
        s_ui.chart = NULL;
        s_ui.chart_head = NULL;
        s_ui.chart_series = NULL;
        s_ui.bar = ui_bar(page, 0);
    }
    s_ui.hint = ui_label(page, "L/R page", 106, UI_BLACK, &lv_font_montserrat_10, LV_TEXT_ALIGN_CENTER);
}

/* Page animation */
static void ui_anim_x(lv_obj_t *obj, int32_t start, int32_t end, lv_anim_completed_cb_t completed_cb)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&a, start, end);
    lv_anim_set_time(&a, 150);
    if (completed_cb) {
        lv_anim_set_completed_cb(&a, completed_cb);
    }
    lv_anim_start(&a);
}

static void ui_show_page(ui_page_t page, int dir)
{
    lv_obj_t *old = s_ui.page;
    const int start_x = dir == 0 ? 0 : (dir > 0 ? LCD_H_RES : -LCD_H_RES);

    s_ui.page_id = page;
    s_ui.page = ui_make_page(start_x);
    s_ui.title = NULL;
    s_ui.value = NULL;
    s_ui.sub = NULL;
    s_ui.bar = NULL;
    s_ui.chart = NULL;
    s_ui.chart_head = NULL;
    s_ui.chart_series = NULL;
    s_ui.chart_history_version = UINT32_MAX;
    s_ui.status = NULL;
    s_ui.hint = NULL;
    s_ui.accent = NULL;
    ui_build_page_content(s_ui.page);
    ui_refresh();

    if (old) {
        if (dir == 0) {
            lv_obj_delete(old);
        }
        else {
            ui_anim_x(old, 0, dir > 0 ? -LCD_H_RES : LCD_H_RES, lv_obj_delete_anim_completed_cb);
        }
    }
    if (dir != 0) {
        ui_anim_x(s_ui.page, start_x, 0, NULL);
    }
}

/* UI refresh: map board state -> widgets */
static void ui_refresh(void)
{
    if (!s_ui.value || !s_ui.sub || !s_ui.hint) {
        return;
    }

    const hw_board_state_t *board = hw_board_state();

    switch (s_ui.page_id) {
    case UI_PAGE_LIGHT:
        lv_label_set_text_fmt(s_ui.value, "%d%%", pct_from_raw(board->light_raw));
        if (board->adc_ready) {
            lv_label_set_text_fmt(s_ui.sub, "GPIO36  RAW %04d", board->light_raw);
        }
        else {
            lv_label_set_text_fmt(s_ui.sub, "ADC FAIL  %s", short_err(board->last_adc_err));
        }
        ui_set_hint("A sample   L/R");
        ui_refresh_history_chart();
        break;
    case UI_PAGE_THERM: {
        const bool therm_changed = s_ui.chart_history_version != s_therm_history_version;
        if (therm_changed || !board->adc_ready || board->last_adc_err != ESP_OK) {
            lv_label_set_text_fmt(s_ui.value, "%d%%", pct_from_raw(board->temp_raw));
            if (board->adc_ready) {
                lv_label_set_text_fmt(s_ui.sub, "GPIO39  RAW %04d", board->temp_raw);
            }
            else {
                lv_label_set_text_fmt(s_ui.sub, "ADC FAIL  %s", short_err(board->last_adc_err));
            }
        }
        ui_set_hint("A sample   L/R");
        ui_refresh_history_chart();
        break;
    }
    case UI_PAGE_MOTION:
        lv_label_set_text(s_ui.value, board->mpu_present ? board->gesture : "ABSENT");
        if (board->mpu_present) {
            lv_label_set_text_fmt(s_ui.sub, "P%+.1f  R%+.1f  0x%02X", board->pitch, board->roll, board->mpu_whoami);
        }
        else {
            lv_label_set_text_fmt(s_ui.sub, "MPU 0x68  %s", short_err(board->last_mpu_err));
        }
        ui_set_hint("A rescan   L/R");
        ui_set_bar(board->mpu_present ? 100 : 0);
        break;
    case UI_PAGE_LED1:
        lv_label_set_text(s_ui.value, board->led1_on ? "ON" : "OFF");
        lv_label_set_text(s_ui.sub, board->gd32_present ? "GD32 0x40  REG A0" : "GD32 0x40 ABSENT");
        ui_set_hint("A toggle   B off");
        ui_set_bar(board->led1_on ? 100 : 0);
        break;
    case UI_PAGE_LED2:
        lv_label_set_text(s_ui.value, board->led2_on ? "ON" : "OFF");
        lv_label_set_text(s_ui.sub, board->gd32_present ? "GD32 0x40  REG A1" : "GD32 0x40 ABSENT");
        ui_set_hint("A toggle   B off");
        ui_set_bar(board->led2_on ? 100 : 0);
        break;
    case UI_PAGE_BUZZER:
        lv_label_set_text_fmt(s_ui.value, "%lu Hz", (unsigned long)s_buzzer_freq_hz);
        lv_label_set_text(s_ui.sub, board->buzzer_ready ? "GPIO14 PWM" : "PWM INIT FAIL");
        ui_set_hint("U/D Hz  A beep  B stop");
        ui_set_bar((int)((s_buzzer_freq_hz - 440) * 100 / (1760 - 440)));
        break;
    case UI_PAGE_MOTOR1:
    case UI_PAGE_MOTOR2: {
        const uint8_t motor = s_ui.page_id == UI_PAGE_MOTOR1 ? 0 : 1;
        lv_label_set_text_fmt(s_ui.value, "%s %03u", board->motor_running[motor] ? "VOUT" : "PWM", board->motor_speed[motor]);
        if (board->gd32_present) {
            lv_label_set_text_fmt(s_ui.sub, "REG %s  DIR %u", motor == 0 ? "0E" : "06", board->motor_dir[motor] ? 1 : 0);
        }
        else {
            lv_label_set_text(s_ui.sub, "GD32 0x40 ABSENT");
        }
        ui_set_hint(board->motor_running[motor] ? "U/D PWM  A off  B stop" : "U/D PWM  A out  B dir");
        ui_set_bar((int)board->motor_speed[motor] * 100 / 255);
        break;
    }
    case UI_PAGE_SD:
        lv_label_set_text(s_ui.value, board->sd_mounted ? "MOUNTED" : "NO CARD");
        if (board->sd_mounted) {
            lv_label_set_text_fmt(s_ui.sub, "%s  %luMB", board->sd_name, (unsigned long)board->sd_mb);
        }
        else {
            lv_label_set_text_fmt(s_ui.sub, "GPIO22 CS  %s", short_err(board->last_sd_err));
        }
        ui_set_hint(board->sd_mounted ? "B unmount  L/R" : "A rescan   L/R");
        ui_set_bar(board->sd_mounted ? 100 : 0);
        break;
    case UI_PAGE_GPIO25:
        lv_label_set_text_fmt(s_ui.value, "%s %03u", board->ext_out[0] ? "PWM" : "OFF", board->ext_pwm[0]);
        lv_label_set_text(s_ui.sub, board->ext_pwm_ready ? "GPIO25 LEDC" : "PWM INIT FAIL");
        ui_set_hint("U/D duty  A toggle  B off");
        ui_set_bar((int)board->ext_pwm[0] * 100 / EXT_PWM_DUTY_MAX);
        break;
    case UI_PAGE_GPIO26:
        lv_label_set_text_fmt(s_ui.value, "%s %03u", board->ext_out[1] ? "PWM" : "OFF", board->ext_pwm[1]);
        lv_label_set_text(s_ui.sub, board->ext_pwm_ready ? "GPIO26 LEDC" : "PWM INIT FAIL");
        ui_set_hint("U/D duty  A toggle  B off");
        ui_set_bar((int)board->ext_pwm[1] * 100 / EXT_PWM_DUTY_MAX);
        break;
    case UI_PAGE_ADC32:
        lv_label_set_text_fmt(s_ui.value, "%d%%", pct_from_raw(board->ext_raw[0]));
        if (board->adc_ready) {
            lv_label_set_text_fmt(s_ui.sub, "GPIO32 RAW %04d", board->ext_raw[0]);
        }
        else {
            lv_label_set_text_fmt(s_ui.sub, "ADC FAIL  %s", short_err(board->last_adc_err));
        }
        ui_set_hint("A sample   L/R");
        ui_set_bar(pct_from_raw(board->ext_raw[0]));
        break;
    case UI_PAGE_ADC33:
        lv_label_set_text_fmt(s_ui.value, "%d%%", pct_from_raw(board->ext_raw[1]));
        if (board->adc_ready) {
            lv_label_set_text_fmt(s_ui.sub, "GPIO33 RAW %04d", board->ext_raw[1]);
        }
        else {
            lv_label_set_text_fmt(s_ui.sub, "ADC FAIL  %s", short_err(board->last_adc_err));
        }
        ui_set_hint("A sample   L/R");
        ui_set_bar(pct_from_raw(board->ext_raw[1]));
        break;
    case UI_PAGE_SYSTEM:
        lv_label_set_text(s_ui.value, board->i2c_ready ? "I2C OK" : "I2C --");
        lv_label_set_text_fmt(s_ui.sub,
                              "G %s  M %s",
                              board->gd32_present ? "OK" : short_err(board->last_gd32_err),
                              board->mpu_present ? "OK" : short_err(board->last_mpu_err));
        ui_set_hint("A rescan   L/R");
        ui_set_bar(board->i2c_ready ? 100 : 0);
        break;
    case UI_PAGE_ABOUT:
        break;
    default:
        break;
    }
}

/* UI action handlers */

static void ui_motor_stop(uint8_t motor)
{
    hw_board_state_t *board = hw_board_state_mut();
    esp_err_t err = hw_gd32_motor_set(motor, board->motor_dir[motor], 0);
    if (err == ESP_OK) {
        board->motor_running[motor] = false;
        set_action(motor == 0 ? "Motor1 stopped" : "Motor2 stopped");
    }
    else {
        set_action("Motor cmd fail");
    }
}

static void ui_motor_toggle(uint8_t motor)
{
    hw_board_state_t *board = hw_board_state_mut();
    if (board->motor_running[motor]) {
        ui_motor_stop(motor);
        return;
    }
    if (board->motor_speed[motor] == 0) {
        board->last_gd32_err = ESP_ERR_INVALID_ARG;
        set_action("PWM is zero");
        return;
    }

    esp_err_t err = hw_gd32_motor_set(motor, board->motor_dir[motor], board->motor_speed[motor]);
    if (err == ESP_OK) {
        board->motor_running[motor] = true;
        set_action(motor == 0 ? "Motor1 output" : "Motor2 output");
    }
    else {
        set_action("Motor cmd fail");
    }
}

static esp_err_t ui_ext_toggle(uint8_t index)
{
    hw_board_state_t *board = hw_board_state_mut();
    if (board->ext_out[index]) {
        esp_err_t err = hw_extio_set(index, false);
        set_action(err == ESP_OK ? (index == 0 ? "GPIO25 off" : "GPIO26 off") : "PWM cmd fail");
        return err;
    }

    if (board->ext_pwm[index] == 0) {
        set_action("Duty is zero");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = hw_extio_set(index, true);
    set_action(err == ESP_OK ? (index == 0 ? "GPIO25 PWM" : "GPIO26 PWM") : "PWM cmd fail");
    return err;
}

static void ui_action(void)
{
    hw_board_state_t *board = hw_board_state_mut();
    esp_err_t err = ESP_OK;

    switch (s_ui.page_id) {
    case UI_PAGE_LIGHT:
    case UI_PAGE_THERM:
    case UI_PAGE_ADC32:
    case UI_PAGE_ADC33:
        hw_adc_update();
        err = board->last_adc_err;
        set_action(err == ESP_OK ? "Sampled" : "ADC read fail");
        break;
    case UI_PAGE_MOTION:
        hw_mpu_probe(true);
        err = board->mpu_present ? ESP_OK : board->last_mpu_err;
        set_action(board->mpu_present ? "MPU ready" : "MPU absent");
        break;
    case UI_PAGE_LED1:
        err = hw_gd32_set_led(0, !board->led1_on);
        set_action(err == ESP_OK ? "LED1 toggled" : "LED cmd fail");
        break;
    case UI_PAGE_LED2:
        err = hw_gd32_set_led(1, !board->led2_on);
        set_action(err == ESP_OK ? "LED2 toggled" : "LED cmd fail");
        break;
    case UI_PAGE_BUZZER:
        hw_buzzer_beep(s_buzzer_freq_hz, 140);
        set_action(board->buzzer_ready ? "Beep" : "Buzzer init fail");
        break;
    case UI_PAGE_MOTOR1:
        ui_motor_toggle(0);
        err = board->last_gd32_err;
        break;
    case UI_PAGE_MOTOR2:
        ui_motor_toggle(1);
        err = board->last_gd32_err;
        break;
    case UI_PAGE_SD:
        hw_sd_try_mount();
        err = board->sd_mounted ? ESP_OK : board->last_sd_err;
        set_action(board->sd_mounted ? "SD mounted" : "No SD card");
        break;
    case UI_PAGE_GPIO25:
        err = ui_ext_toggle(0);
        break;
    case UI_PAGE_GPIO26:
        err = ui_ext_toggle(1);
        break;
    case UI_PAGE_SYSTEM:
        hw_gd32_probe(true);
        hw_mpu_probe(true);
        err = board->i2c_ready ? ESP_OK : ESP_ERR_INVALID_STATE;
        set_action(board->i2c_ready ? "Rescanned" : "I2C init fail");
        break;
    default:
        break;
    }

    if (err == ESP_OK && s_ui.page_id != UI_PAGE_BUZZER) {
        hw_buzzer_beep(660, 35);
    }
    ui_refresh();
}

static void ui_cancel(void)
{
    hw_board_state_t *board = hw_board_state_mut();

    switch (s_ui.page_id) {
    case UI_PAGE_LED1:
        if (board->led1_on) {
            esp_err_t err = hw_gd32_set_led(0, false);
            set_action(err == ESP_OK ? "LED1 off" : "LED cmd fail");
        }
        else {
            set_action("LED1 off");
        }
        break;
    case UI_PAGE_LED2:
        if (board->led2_on) {
            esp_err_t err = hw_gd32_set_led(1, false);
            set_action(err == ESP_OK ? "LED2 off" : "LED cmd fail");
        }
        else {
            set_action("LED2 off");
        }
        break;
    case UI_PAGE_BUZZER:
        hw_buzzer_stop();
        set_action("Buzzer stop");
        break;
    case UI_PAGE_SD:
        hw_sd_unmount();
        break;
    case UI_PAGE_GPIO25:
        {
            esp_err_t err = hw_extio_set(0, false);
            set_action(err == ESP_OK ? "GPIO25 off" : "PWM cmd fail");
        }
        break;
    case UI_PAGE_GPIO26:
        {
            esp_err_t err = hw_extio_set(1, false);
            set_action(err == ESP_OK ? "GPIO26 off" : "PWM cmd fail");
        }
        break;
    case UI_PAGE_MOTOR1:
        if (board->motor_running[0]) {
            ui_motor_stop(0);
        }
        else {
            board->motor_dir[0] = !board->motor_dir[0];
            set_action("Motor1 dir");
        }
        break;
    case UI_PAGE_MOTOR2:
        if (board->motor_running[1]) {
            ui_motor_stop(1);
        }
        else {
            board->motor_dir[1] = !board->motor_dir[1];
            set_action("Motor2 dir");
        }
        break;
    default:
        hw_buzzer_stop();
        set_action("Canceled");
        break;
    }
    ui_refresh();
}

static void ui_adjust(int step)
{
    hw_board_state_t *board = hw_board_state_mut();

    switch (s_ui.page_id) {
    case UI_PAGE_BUZZER: {
        int freq = (int)s_buzzer_freq_hz + step * 110;
        s_buzzer_freq_hz = MAX(440, MIN(freq, 1760));
        set_action("Pitch set");
        break;
    }
    case UI_PAGE_MOTOR1:
    case UI_PAGE_MOTOR2: {
        const uint8_t motor = s_ui.page_id == UI_PAGE_MOTOR1 ? 0 : 1;
        int speed = board->motor_speed[motor] + step * 10;
        board->motor_speed[motor] = MAX(0, MIN(speed, 255));
        if (board->motor_running[motor]) {
            esp_err_t err = hw_gd32_motor_set(motor, board->motor_dir[motor], board->motor_speed[motor]);
            set_action(err == ESP_OK ? "Power set" : "Motor cmd fail");
        }
        else {
            set_action("Power set");
        }
        break;
    }
    case UI_PAGE_GPIO25:
    case UI_PAGE_GPIO26: {
        const uint8_t index = s_ui.page_id == UI_PAGE_GPIO25 ? 0 : 1;
        int duty = board->ext_pwm[index] + step * 16;
        board->ext_pwm[index] = MAX(0, MIN(duty, EXT_PWM_DUTY_MAX));
        if (!board->ext_pwm_ready) {
            set_action("PWM init fail");
        }
        else if (board->ext_out[index]) {
            esp_err_t err = hw_extio_set(index, true);
            set_action(err == ESP_OK ? (board->ext_out[index] ? "Duty set" : "Duty zero") : "PWM cmd fail");
        }
        else {
            set_action("Duty set");
        }
        break;
    }
    default:
        return;
    }
    ui_refresh();
}

static void ui_scroll_about(int step)
{
    if (!s_ui.page) {
        return;
    }

    const int32_t scroll_step = 26;
    const int32_t scroll_y = lv_obj_get_scroll_y(s_ui.page) + step * scroll_step;
    lv_obj_scroll_to_y(s_ui.page, MAX(0, scroll_y), LV_ANIM_ON);
}

static void ui_key_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_KEY) {
        return;
    }

    const uint32_t key = lv_event_get_key(e);

    if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
        int next = (int)s_ui.page_id + (key == LV_KEY_RIGHT ? 1 : -1);
        if (next < 0) {
            next = UI_PAGE_COUNT - 1;
        }
        if (next >= UI_PAGE_COUNT) {
            next = 0;
        }
        ui_show_page((ui_page_t)next, key == LV_KEY_RIGHT ? 1 : -1);
    }
    else if (key == LV_KEY_UP) {
        if (s_ui.page_id == UI_PAGE_ABOUT) {
            ui_scroll_about(-1);
        }
        else {
            ui_adjust(1);
        }
    }
    else if (key == LV_KEY_DOWN) {
        if (s_ui.page_id == UI_PAGE_ABOUT) {
            ui_scroll_about(1);
        }
        else {
            ui_adjust(-1);
        }
    }
    else if (key == LV_KEY_ENTER) {
        ui_action();
    }
    else if (key == LV_KEY_ESC) {
        ui_cancel();
    }
}

static void ui_create(lv_group_t *group)
{
    s_ui.group = group;
    s_ui.page_id = UI_PAGE_LIGHT;
    s_ui.screen = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_ui.screen);
    lv_obj_set_size(s_ui.screen, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(s_ui.screen, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_bg_opa(s_ui.screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.screen, LV_OBJ_FLAG_CLICKABLE);
    lv_group_add_obj(group, s_ui.screen);
    lv_group_focus_obj(s_ui.screen);
    lv_obj_add_event_cb(s_ui.screen, ui_key_event_cb, LV_EVENT_KEY, NULL);
    ui_show_page(UI_PAGE_LIGHT, 0);
}

/* LVGL task */
static void lvgl_task(void *arg)
{
    lv_group_t *group = (lv_group_t *)arg;
    uint32_t last_update_ms = 0;

    ESP_LOGI(TAG, "Start Xiaomiao hardware dashboard");
    ui_create(group);
    lv_refr_now(NULL);
    for (uint8_t i = 0; i < 100 && !hw_display_first_flush_done(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    hw_display_on();

    while (true) {
        hw_board_process_timers();
        if (lv_tick_elaps(last_update_ms) >= UI_REFRESH_PERIOD_MS) {
            last_update_ms = lv_tick_get();
            hw_board_update();
            accumulate_adc_to_history();
            ui_refresh();
        }

        uint32_t delay_ms = lv_timer_handler();
        delay_ms = MAX(delay_ms, LVGL_TASK_MIN_DELAY_MS);
        delay_ms = MIN(delay_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(delay_ms * 1000);
    }
}
#endif /* !CONFIG_XIAOMIAO_USE_SDL */

/* ---- WAMR Canvas 2D test task (runs in pthread; wasm_runtime_full_init
 *      internally calls pthread_self, which needs pthread TLS). ---- */
static void *wasm_test_task(void *arg)
{
    (void)arg;
    ESP_LOGI("wasm", "wasm_test_task enter");

    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = malloc(256 * 1024);
    init_args.mem_alloc_option.pool.heap_size = 256 * 1024;

    if (!wasm_runtime_full_init(&init_args)) {
        ESP_LOGE("wasm", "WAMR runtime init failed");
        free(init_args.mem_alloc_option.pool.heap_buf);
        return NULL;
    }
    ESP_LOGI("wasm", "WAMR runtime initialized");

    hw_fb_init();
    canvas_api_init(hw_fb_buffer(), 160, 128);
    if (!canvas_api_register()) {
        ESP_LOGE("wasm", "Canvas API registration failed");
    } else {
        ESP_LOGI("wasm", "Canvas 2D API registered");
    }
    hw_display_on();  /* Turn on ST7735 after first framebuffer is ready */
    input_api_register();
    audio_api_register();
    ESP_LOGI("wasm", "host APIs registered, about to load .wasm");

    /* Load WASM bytecode */
    char error_buf[128];
    wasm_module_t module = wasm_runtime_load(wasm_test_wasm, wasm_test_wasm_len,
                                               error_buf, sizeof(error_buf));
    if (!module) {
        ESP_LOGE("wasm", "Load failed: %s", error_buf);
        wasm_runtime_destroy();
        return NULL;
    }
    ESP_LOGI("wasm", "WASM module loaded (%u bytes)", wasm_test_wasm_len);

    /* Instantiate */
    wasm_module_inst_t inst = wasm_runtime_instantiate(module, 8192, 0,
                                                        error_buf, sizeof(error_buf));
    if (!inst) {
        ESP_LOGE("wasm", "Instantiate failed: %s", error_buf);
        wasm_runtime_unload(module);
        wasm_runtime_destroy();
        return NULL;
    }
    ESP_LOGI("wasm", "Instance created");

    wasm_function_inst_t func = wasm_runtime_lookup_function(inst, "draw");
    if (!func) {
        ESP_LOGE("wasm", "Function 'draw' not found");
        wasm_runtime_deinstantiate(inst);
        wasm_runtime_unload(module);
        wasm_runtime_destroy();
        return NULL;
    }
    ESP_LOGI("wasm", "draw() found, calling...");

    wasm_exec_env_t exec_env = wasm_runtime_create_exec_env(inst, 8192);
    if (!exec_env) {
        ESP_LOGE("wasm", "Create exec env failed");
        wasm_runtime_deinstantiate(inst);
        wasm_runtime_unload(module);
        wasm_runtime_destroy();
        return NULL;
    }

    if (!wasm_runtime_call_wasm(exec_env, func, 0, NULL)) {
        ESP_LOGE("wasm", "Call 'draw' failed: %s",
                 wasm_runtime_get_exception(inst));
    } else {
        ESP_LOGI("wasm", "draw() returned successfully");
    }

    wasm_runtime_destroy_exec_env(exec_env);
    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(module);
    wasm_runtime_destroy();
    ESP_LOGI("wasm", "wasm_test_task done");
    return NULL;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Xiaomiao SDL3 demo boot");

    /* Hardware init (SPI2, I2C0, ADC, buzzer, ext-IO, display, buttons) */
    hw_board_init();

    /* adb-like serial debug console (REPL on UART0, independent task).
     * Spawned before the blocking benchmark join so the prompt is available
     * while the 3D render loop runs; GL state is NULL until glInit. */
    debug_console_start();

    /* ── WASM 3D Game Demo ──────────────────────────────────
     * Core 0: WAMR wasm game logic + physics
     * Core 1: TinyGL native rendering
     *
     * The wasm module calls render_submit_cube() / physics_step()
     * host functions; core 1 drains the render queue each frame. */
    {
        RuntimeInitArgs init_args;
        memset(&init_args, 0, sizeof(init_args));
        init_args.mem_alloc_type = Alloc_With_Pool;
        init_args.mem_alloc_option.pool.heap_buf = malloc(256 * 1024);
        init_args.mem_alloc_option.pool.heap_size = 256 * 1024;

        if (!wasm_runtime_full_init(&init_args)) {
            ESP_LOGE(TAG, "WAMR init failed");
            free(init_args.mem_alloc_option.pool.heap_buf);
            return;
        }
        ESP_LOGI(TAG, "WAMR runtime initialized");

        /* Register render + physics Host API */
        if (!render_api_register()) {
            ESP_LOGE(TAG, "Render API registration failed");
        }

        /* Initialize TinyGL pipeline (display + textures + lighting) */
        extern const display_backend_t st7735_display_backend;
        const display_backend_t *disp = &st7735_display_backend;
        disp->init(160, 128, PIXEL_FORMAT_RGB565_SWAP);
        if (gl_init(160, 128) != 0) {
            ESP_LOGE(TAG, "gl_init failed");
        }

        physics_init();

        /* Load and instantiate the wasm game module */
        char error_buf[128];
#ifdef CONFIG_WAT2WASM_AVAILABLE
        #include "wasm_game.wasm.h"
        wasm_module_t module = wasm_runtime_load(wasm_game_wasm, wasm_game_wasm_len,
                                                   error_buf, sizeof(error_buf));
#else
        #include "wasm_test.wasm.h"
        wasm_module_t module = wasm_runtime_load(wasm_test_wasm, wasm_test_wasm_len,
                                                   error_buf, sizeof(error_buf));
#endif
        if (!module) {
            ESP_LOGE(TAG, "WASM load failed: %s", error_buf);
            wasm_runtime_destroy();
            return;
        }
        ESP_LOGI(TAG, "WASM module loaded");

        wasm_module_inst_t inst = wasm_runtime_instantiate(module, 16384, 0,
                                                            error_buf, sizeof(error_buf));
        if (!inst) {
            ESP_LOGE(TAG, "Instantiate failed: %s", error_buf);
            wasm_runtime_unload(module);
            wasm_runtime_destroy();
            return;
        }
        ESP_LOGI(TAG, "WASM instance created");

        /* Call game_init() once */
        wasm_function_inst_t func_init = wasm_runtime_lookup_function(inst, "game_init");
        if (func_init) {
            wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 8192);
            if (env) {
                wasm_runtime_call_wasm(env, func_init, 0, NULL);
                wasm_runtime_destroy_exec_env(env);
                ESP_LOGI(TAG, "game_init() called");
            }
        }

        wasm_function_inst_t func_update = wasm_runtime_lookup_function(inst, "game_update");
        if (!func_update) {
            ESP_LOGE(TAG, "game_update() not found");
        } else {
            ESP_LOGI(TAG, "game_update() found — entering game loop");

            /* Pin render loop to core 1 */
            xTaskCreatePinnedToCore(
                tinygl_render_task,
                "tinygl_render",
                8192,
                NULL,
                configMAX_PRIORITIES - 1,
                NULL,
                1  /* core 1 */
            );

            /* Core 0: game loop — call wasm game_update() at ~30 Hz */
            int64_t last_us = esp_timer_get_time();
            while (1) {
                wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 8192);
                if (env) {
                    wasm_runtime_call_wasm(env, func_update, 0, NULL);
                    wasm_runtime_destroy_exec_env(env);
                }

                /* 30 Hz frame pacing */
                int64_t now_us = esp_timer_get_time();
                int64_t elapsed_us = now_us - last_us;
                last_us = now_us;
                if (elapsed_us < 33333) {
                    vTaskDelay(pdMS_TO_TICKS((33333 - elapsed_us) / 1000));
                } else {
                    vTaskDelay(1);
                }
            }
        }
    }
}