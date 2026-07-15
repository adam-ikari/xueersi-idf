/**
 * @file sdl_demo.c
 * @brief SDL3 15-page hardware self-test demo for Xiaomiao handheld
 *
 * Full pager with page navigation, slide animation, button input
 * via SDL event injection, history charts, and per-page hardware
 * interaction (LED, motor, buzzer, SD, GPIO, ADC, MPU).
 *
 * Replace the /tmp copy after every Edit outside the worktree.
 */

#include "sdl_demo.h"

#include "hw_board.h"
#include "hw_adc.h"
#include "hw_display.h"
#include "hw_input.h"
#include "hw_i2c.h"
#include "hw_gd32.h"
#include "hw_mpu.h"
#include "hw_buzzer.h"
#include "hw_extio.h"
#include "hw_sd.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

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
#define UI_TARGET_NAME "esp32"
#else
#define UI_TARGET_NAME CONFIG_IDF_TARGET
#endif

/* ── Tuning constants ─────────────────────────────────────── */
#define UI_REFRESH_PERIOD_MS    16
#define UI_HISTORY_POINTS       48
#define UI_ACTION_MSG_MS        850
#define UI_SLIDE_FRAMES         4
#define UI_SLIDE_PX             16

/* ── Page names ───────────────────────────────────────────── */
static const char *const s_page_names[UI_PAGE_COUNT] = {
    "LIGHT", "THERM", "MOTION", "LED 1", "LED 2", "BUZZER",
    "MOTOR 1", "MOTOR 2", "SD CARD", "GPIO25", "GPIO26",
    "GPIO32", "GPIO33", "SYSTEM", "ABOUT",
};

/* ── Colors (individual r/g/b -- not macros to avoid comma-expansion issues) ─── */
static const uint8_t C_BG_R = 0x1B, C_BG_G = 0x17, C_BG_B = 0x13;
static const uint8_t C_TITLE_R = 0xF6, C_TITLE_G = 0xD3, C_TITLE_B = 0x4A;
static const uint8_t C_ACC_R = 0xE6, C_ACC_G = 0x4B, C_ACC_B = 0x3C;
static const uint8_t C_BAR_BG_R = 0xFF, C_BAR_BG_G = 0xF3, C_BAR_BG_B = 0xB0;
static const uint8_t C_BAR_FILL_R = 0x5C, C_BAR_FILL_G = 0x42, C_BAR_FILL_B = 0x20;
static const uint8_t C_TEXT_R = 0xFF, C_TEXT_G = 0xFF, C_TEXT_B = 0xFF;
static const uint8_t C_SUB_R = 0xBB, C_SUB_G = 0xBB, C_SUB_B = 0xBB;

/* ── SDL3 globals ─────────────────────────────────────────── */
static SDL_Surface  *s_surface  = NULL;
static SDL_Renderer *s_renderer = NULL;
static const char   *TAG        = "sdl_demo";

/* ── UI state ─────────────────────────────────────────────── */
static ui_page_t        s_current_page = UI_PAGE_LIGHT;
static int              s_slide_offset = 0;
static int              s_slide_target = 0;
static int              s_slide_frames_remaining = 0;
static uint32_t         s_action_until_ms = 0;
static uint32_t         s_buzzer_freq_hz = 988;
static char             s_action_msg[32] = "";

/* ── History arrays (ring buffers) ────────────────────────── */
static int32_t   s_light_history[UI_HISTORY_POINTS];
static int32_t   s_therm_history[UI_HISTORY_POINTS];
static uint32_t  s_light_history_version;
static uint32_t  s_therm_history_version;
static uint32_t  s_light_hist_accum;
static uint8_t   s_light_hist_count;
static uint32_t  s_therm_accum;
static uint16_t  s_therm_accum_count;
static uint32_t  s_last_therm_publish_ms;

/* ── Button debounce tracking ─────────────────────────────── */
static bool s_btn_prev[6] = {false, false, false, false, false, false};

/* ── SDL scancode mapping for each button ─────────────────── */
static const SDL_Scancode s_btn_scancode[6] = {
    SDL_SCANCODE_UP,     /* BUTTON_IDX_UP    */
    SDL_SCANCODE_DOWN,   /* BUTTON_IDX_DOWN  */
    SDL_SCANCODE_LEFT,   /* BUTTON_IDX_LEFT  */
    SDL_SCANCODE_RIGHT,  /* BUTTON_IDX_RIGHT */
    SDL_SCANCODE_RETURN, /* BUTTON_IDX_A     */
    SDL_SCANCODE_ESCAPE, /* BUTTON_IDX_B     */
};

/* ── Forward declarations ─────────────────────────────────── */
static void set_action(const char *msg);
static void sensor_history_init(void);
static void sensor_history_push(int32_t *history, uint32_t *version, int value);
static void accumulate_adc_to_history(void);
static void render_page(const hw_board_state_t *board);
static void render_title_bar(ui_page_t page);
static void render_bar(int y, int pct);
static void render_line_chart(int32_t *history, int y, int h);
static void render_action_hint(const hw_board_state_t *board);
static void render_status_line(const hw_board_state_t *board);
static void render_debug_text(int y, const char *text, uint8_t r, uint8_t g, uint8_t b);
static void handle_action(void);
static void handle_cancel(void);
static void handle_adjust(int step);
static void inject_button_events(void);
static void copy_text(char *dst, size_t dst_size, const char *src);
static unsigned ui_kb(size_t bytes);

/* ============================================================
 *  Helpers
 * ============================================================ */
static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static void set_action(const char *msg)
{
    copy_text(s_action_msg, sizeof(s_action_msg), msg);
    s_action_until_ms = SDL_GetTicks() + UI_ACTION_MSG_MS;
}

static unsigned ui_kb(size_t bytes)
{
    return (unsigned)((bytes + 512) / 1024);
}

/* ============================================================
 *  Sensor history
 * ============================================================ */
static void sensor_history_init(void)
{
    for (size_t i = 0; i < UI_HISTORY_POINTS; i++) {
        s_light_history[i] = -1;
        s_therm_history[i] = -1;
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
    for (size_t i = 1; i < UI_HISTORY_POINTS; i++) {
        history[i - 1] = history[i];
    }
    history[UI_HISTORY_POINTS - 1] = MAX(0, MIN(value, 100));
    (*version)++;
}

static void accumulate_adc_to_history(void)
{
    const hw_board_state_t *board = hw_board_state();
    s_light_hist_accum += pct_from_raw(board->light_raw);
    s_light_hist_count++;
    if (s_light_hist_count >= 4) {
        sensor_history_push(s_light_history,
                            &s_light_history_version,
                            (int)((s_light_hist_accum + s_light_hist_count / 2) / s_light_hist_count));
        s_light_hist_accum = 0;
        s_light_hist_count = 0;
    }

    const uint32_t now_ms = SDL_GetTicks();
    s_therm_accum += board->temp_raw;
    s_therm_accum_count++;
    if (s_last_therm_publish_ms == 0 || now_ms - s_last_therm_publish_ms >= 1000) {
        const int avg_raw = (int)((s_therm_accum + s_therm_accum_count / 2) / s_therm_accum_count);
        int pct = pct_from_raw(avg_raw);
        pct = MAX(35, MIN(pct, 65));
        sensor_history_push(s_therm_history, &s_therm_history_version, pct);
        s_therm_accum = 0;
        s_therm_accum_count = 0;
        s_last_therm_publish_ms = now_ms;
    }
}

/* ============================================================
 *  SDL event injection: poll GPIO buttons -> push SDL events
 * ============================================================ */
static void inject_button_events(void)
{
    SDL_Event ev;
    SDL_zero(ev);

    const size_t btn_count = 6;
    for (size_t i = 0; i < btn_count; i++) {
        bool pressed = hw_input_is_pressed(i);
        if (pressed && !s_btn_prev[i]) {
            /* rising edge: key down */
            ev.type = SDL_EVENT_KEY_DOWN;
            ev.key.scancode = s_btn_scancode[i];
            ev.key.key = 0;
            SDL_PushEvent(&ev);
        }
        else if (!pressed && s_btn_prev[i]) {
            /* falling edge: key up */
            ev.type = SDL_EVENT_KEY_UP;
            ev.key.scancode = s_btn_scancode[i];
            ev.key.key = 0;
            SDL_PushEvent(&ev);
        }
        s_btn_prev[i] = pressed;
    }
}

/* ============================================================
 *  Drawing primitives
 * ============================================================ */

/** Draw a progress bar at y=row_y, width=152, height=10, centered at x=4 */
static void render_bar(int y, int pct)
{
    pct = MAX(0, MIN(pct, 100));
    const int bar_x = 4;
    const int bar_w = 152;
    const int bar_h = 10;

    /* background (cream) */
    SDL_SetRenderDrawColor(s_renderer, C_BAR_BG_R, C_BAR_BG_G, C_BAR_BG_B, 0xFF);
    SDL_RenderFillRect(s_renderer, &(SDL_FRect){bar_x, y, bar_w, bar_h});

    /* fill (brown) */
    int fill_w = bar_w * pct / 100;
    if (fill_w > 0) {
        SDL_SetRenderDrawColor(s_renderer, C_BAR_FILL_R, C_BAR_FILL_G, C_BAR_FILL_B, 0xFF);
    SDL_RenderFillRect(s_renderer, &(SDL_FRect){bar_x, y, fill_w, bar_h});
    }
}

/** Draw a simple line chart from a history array */
static void render_line_chart(int32_t *history, int y, int h)
{
    int chart_w = 152;
    int x0 = 4;
    int y0 = y + h;  /* bottom */

    /* frame */
    SDL_SetRenderDrawColor(s_renderer, C_SUB_R, C_SUB_G, C_SUB_B, 0xFF);
    SDL_RenderRect(s_renderer, &(SDL_FRect){x0, y, chart_w, h});

    /* lines */
    if (history[0] < 0) return;

    SDL_SetRenderDrawColor(s_renderer, C_ACC_R, C_ACC_G, C_ACC_B, 0xFF);
    float prev_x = (float)x0;
    float prev_y = (float)(y0 - (history[0] * h / 100));
    for (size_t i = 1; i < UI_HISTORY_POINTS; i++) {
        if (history[i] < 0) break;
        float cx = (float)(x0 + i * chart_w / (UI_HISTORY_POINTS - 1));
        float cy = (float)(y0 - (history[i] * h / 100));
        SDL_RenderLine(s_renderer, prev_x, prev_y, cx, cy);
        prev_x = cx;
        prev_y = cy;
    }
}

/** Render debuggable text at a fixed y position */
static void render_debug_text(int y, const char *text, uint8_t r, uint8_t g, uint8_t b)
{
    if (!text || !text[0]) return;
    SDL_SetRenderDrawColor(s_renderer, r, g, b, 0xFF);
    SDL_RenderDebugText(s_renderer, 4.0f, (float)y, text);
}

/** Render title bar: page name + page index */
static void render_title_bar(ui_page_t page)
{
    char title_buf[32];
    snprintf(title_buf, sizeof(title_buf), "%s", s_page_names[page]);
    render_debug_text(2, title_buf, C_TITLE_R, C_TITLE_G, C_TITLE_B);

    char idx_buf[8];
    snprintf(idx_buf, sizeof(idx_buf), "%02u/%02u", (unsigned)page + 1, (unsigned)UI_PAGE_COUNT);
    SDL_SetRenderDrawColor(s_renderer, C_BAR_FILL_R, C_BAR_FILL_G, C_BAR_FILL_B, 0xFF);
    SDL_RenderDebugText(s_renderer, 160.0f - 8.0f * strlen(idx_buf), 2.0f, idx_buf);

    /* accent dot at (132, 25) - small red circle */
    SDL_SetRenderDrawColor(s_renderer, C_ACC_R, C_ACC_G, C_ACC_B, 0xFF);
    SDL_RenderFillRect(s_renderer, &(SDL_FRect){132.0f, 25.0f, 7.0f, 7.0f});
}

/** Render action hint at bottom */
static void render_action_hint(const hw_board_state_t *board)
{
    uint32_t now = SDL_GetTicks();
    const char *msg;
    if (s_action_until_ms && (int32_t)(s_action_until_ms - now) > 0) {
        msg = s_action_msg;
    }
    else {
        s_action_until_ms = 0;
        s_action_msg[0] = '\0';
        msg = NULL;
    }
    if (msg && msg[0]) {
        render_debug_text(116, msg, C_ACC_R, C_ACC_G, C_ACC_B);
    }
}

/** Render status line at bottom */
static void render_status_line(const hw_board_state_t *board)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "G:%s M:%s SD:%s",
             board->gd32_present ? "OK" : "--",
             board->mpu_present ? "OK" : "--",
             board->sd_mounted ? board->sd_name : "---");
    render_debug_text(106, buf, C_SUB_R, C_SUB_G, C_SUB_B);
}

/* ============================================================
 *  Page render dispatcher
 * ============================================================ */
static void render_page(const hw_board_state_t *board)
{
    char buf[64];

    /* Clear */
    SDL_SetRenderDrawColor(s_renderer, C_BG_R, C_BG_G, C_BG_B, 0xFF);
    SDL_RenderClear(s_renderer);

    /* Apply slide offset */
    int slide = 0;
    if (s_slide_frames_remaining > 0) {
        slide = s_slide_offset;
    }

    /* We render into a temporary context shifted by slide.
     * SDL3 doesn't have push/pop clip, so we just add offset
     * to all x coordinates. We use a simple approach:
     * keep content_x as 4 + slide for first column. */
    int content_x = 4 + slide;

    /* Title bar (static, not shifted) */
    render_title_bar(s_current_page);

    switch (s_current_page) {
    /* ── LIGHT ───────────────────────────────────────────── */
    case UI_PAGE_LIGHT:
        snprintf(buf, sizeof(buf), "%d%%", pct_from_raw(board->light_raw));
        render_debug_text(18, buf, C_TEXT_R, C_TEXT_G, C_TEXT_B);

        if (board->adc_ready) {
            snprintf(buf, sizeof(buf), "GPIO36  RAW %04d", board->light_raw);
        }
        else {
            snprintf(buf, sizeof(buf), "ADC FAIL");
        }
        render_debug_text(30, buf, C_SUB_R, C_SUB_G, C_SUB_B);

        render_bar(42, pct_from_raw(board->light_raw));
        render_line_chart(s_light_history, 54, 48);
        render_debug_text(104, "A sample   L/R", C_SUB_R, C_SUB_G, C_SUB_B);
        break;

    /* ── THERM ───────────────────────────────────────────── */
    case UI_PAGE_THERM:
        snprintf(buf, sizeof(buf), "%d%%", pct_from_raw(board->temp_raw));
        render_debug_text(18, buf, C_TEXT_R, C_TEXT_G, C_TEXT_B);

        if (board->adc_ready) {
            snprintf(buf, sizeof(buf), "GPIO39  RAW %04d", board->temp_raw);
        }
        else {
            snprintf(buf, sizeof(buf), "ADC FAIL");
        }
        render_debug_text(30, buf, C_SUB_R, C_SUB_G, C_SUB_B);

        render_bar(42, pct_from_raw(board->temp_raw));
        render_line_chart(s_therm_history, 54, 48);
        render_debug_text(104, "A sample   L/R", C_SUB_R, C_SUB_G, C_SUB_B);
        break;

    /* ── MOTION ──────────────────────────────────────────── */
    case UI_PAGE_MOTION:
        if (board->mpu_present) {
            render_debug_text(18, board->gesture, C_TEXT_R, C_TEXT_G, C_TEXT_B);
            snprintf(buf, sizeof(buf), "P%+.1f  R%+.1f", board->pitch, board->roll);
            render_debug_text(30, buf, C_SUB_R, C_SUB_G, C_SUB_B);
            snprintf(buf, sizeof(buf), "ACC X%+4d Y%+4d Z%+4d", board->acc[0], board->acc[1], board->acc[2]);
            render_debug_text(44, buf, C_TEXT_R, C_TEXT_G, C_TEXT_B);
            snprintf(buf, sizeof(buf), "GYR X%+4d Y%+4d Z%+4d", board->gyro[0], board->gyro[1], board->gyro[2]);
            render_debug_text(56, buf, C_TEXT_R, C_TEXT_G, C_TEXT_B);
            snprintf(buf, sizeof(buf), "WHO_AM_I: 0x%02X", board->mpu_whoami);
            render_debug_text(68, buf, C_SUB_R, C_SUB_G, C_SUB_B);
        }
        else {
            render_debug_text(18, "ABSENT", C_ACC_R, C_ACC_G, C_ACC_B);
            render_debug_text(30, "MPU 0x68  not found", C_SUB_R, C_SUB_G, C_SUB_B);
        }
        render_bar(80, board->mpu_present ? 100 : 0);
        render_debug_text(104, "A rescan   L/R", C_SUB_R, C_SUB_G, C_SUB_B);
        break;

    /* ── LED 1 ───────────────────────────────────────────── */
    case UI_PAGE_LED1:
        render_debug_text(18, board->led1_on ? "ON" : "OFF", C_TEXT_R, C_TEXT_G, C_TEXT_B);
        snprintf(buf, sizeof(buf), "GD32 0x40  REG 0xA0");
        render_debug_text(30, buf, C_SUB_R, C_SUB_G, C_SUB_B);
        render_bar(44, board->led1_on ? 100 : 0);
        render_debug_text(104, "A toggle   B off", C_SUB_R, C_SUB_G, C_SUB_B);
        break;

    /* ── LED 2 ───────────────────────────────────────────── */
    case UI_PAGE_LED2:
        render_debug_text(18, board->led2_on ? "ON" : "OFF", C_TEXT_R, C_TEXT_G, C_TEXT_B);
        snprintf(buf, sizeof(buf), "GD32 0x40  REG 0xA1");
        render_debug_text(30, buf, C_SUB_R, C_SUB_G, C_SUB_B);
        render_bar(44, board->led2_on ? 100 : 0);
        render_debug_text(104, "A toggle   B off", C_SUB_R, C_SUB_G, C_SUB_B);
        break;

    /* ── BUZZER ──────────────────────────────────────────── */
    case UI_PAGE_BUZZER:
        snprintf(buf, sizeof(buf), "%lu Hz", (unsigned long)s_buzzer_freq_hz);
        render_debug_text(18, buf, C_TEXT_R, C_TEXT_G, C_TEXT_B);
        render_debug_text(30, board->buzzer_ready ? "GPIO14 PWM" : "PWM INIT FAIL", C_SUB_R, C_SUB_G, C_SUB_B);
        render_bar(44, (int)((s_buzzer_freq_hz - 440) * 100 / (1760 - 440)));
        render_debug_text(104, "U/D Hz  A beep  B stop", C_SUB_R, C_SUB_G, C_SUB_B);
        break;

    /* ── MOTOR 1 / MOTOR 2 ───────────────────────────────── */
    case UI_PAGE_MOTOR1:
    case UI_PAGE_MOTOR2: {
        const uint8_t motor = (s_current_page == UI_PAGE_MOTOR1) ? 0 : 1;
        snprintf(buf, sizeof(buf), "%s %03u",
                 board->motor_running[motor] ? "VOUT" : "PWM",
                 board->motor_speed[motor]);
        render_debug_text(18, buf, C_TEXT_R, C_TEXT_G, C_TEXT_B);
        if (board->gd32_present) {
            snprintf(buf, sizeof(buf), "REG %s  DIR %u",
                     motor == 0 ? "0E" : "06",
                     board->motor_dir[motor] ? 1 : 0);
        }
        else {
            snprintf(buf, sizeof(buf), "GD32 0x40 ABSENT");
        }
        render_debug_text(30, buf, C_SUB_R, C_SUB_G, C_SUB_B);
        render_bar(44, (int)board->motor_speed[motor] * 100 / 255);
        render_debug_text(104,
                          board->motor_running[motor]
                              ? "U/D PWM  A off  B stop"
                              : "U/D PWM  A out  B dir",
                          C_SUB_R, C_SUB_G, C_SUB_B);
        break;
    }

    /* ── SD CARD ─────────────────────────────────────────── */
    case UI_PAGE_SD:
        if (board->sd_mounted) {
            snprintf(buf, sizeof(buf), "%s", board->sd_name);
            render_debug_text(18, buf, C_TEXT_R, C_TEXT_G, C_TEXT_B);
            snprintf(buf, sizeof(buf), "%lu MB", (unsigned long)board->sd_mb);
            render_debug_text(30, buf, C_SUB_R, C_SUB_G, C_SUB_B);
            render_bar(44, 100);
        }
        else {
            render_debug_text(18, "NO CARD", C_ACC_R, C_ACC_G, C_ACC_B);
            render_debug_text(30, "SPI2 CS22  SDIO", C_SUB_R, C_SUB_G, C_SUB_B);
            render_bar(44, 0);
        }
        render_debug_text(104, "A mount   B unmount   L/R", C_SUB_R, C_SUB_G, C_SUB_B);
        break;

    /* ── GPIO25 ──────────────────────────────────────────── */
    case UI_PAGE_GPIO25:
        snprintf(buf, sizeof(buf), "%s  %u",
                 board->ext_out[0] ? "ON" : "OFF",
                 board->ext_pwm[0]);
        render_debug_text(18, buf, C_TEXT_R, C_TEXT_G, C_TEXT_B);
        render_debug_text(30, board->ext_pwm_ready ? "GPIO25 LEDC" : "PWM INIT FAIL", C_SUB_R, C_SUB_G, C_SUB_B);
        render_bar(44, (int)board->ext_pwm[0] * 100 / EXT_PWM_DUTY_MAX);
        render_debug_text(104, "U/D duty  A toggle  B off", C_SUB_R, C_SUB_G, C_SUB_B);
        break;

    /* ── GPIO26 ──────────────────────────────────────────── */
    case UI_PAGE_GPIO26:
        snprintf(buf, sizeof(buf), "%s  %u",
                 board->ext_out[1] ? "ON" : "OFF",
                 board->ext_pwm[1]);
        render_debug_text(18, buf, C_TEXT_R, C_TEXT_G, C_TEXT_B);
        render_debug_text(30, board->ext_pwm_ready ? "GPIO26 LEDC" : "PWM INIT FAIL", C_SUB_R, C_SUB_G, C_SUB_B);
        render_bar(44, (int)board->ext_pwm[1] * 100 / EXT_PWM_DUTY_MAX);
        render_debug_text(104, "U/D duty  A toggle  B off", C_SUB_R, C_SUB_G, C_SUB_B);
        break;

    /* ── ADC32 ───────────────────────────────────────────── */
    case UI_PAGE_ADC32:
        snprintf(buf, sizeof(buf), "%d%%", pct_from_raw(board->ext_raw[0]));
        render_debug_text(18, buf, C_TEXT_R, C_TEXT_G, C_TEXT_B);
        if (board->adc_ready) {
            snprintf(buf, sizeof(buf), "GPIO32 RAW %04d", board->ext_raw[0]);
        }
        else {
            snprintf(buf, sizeof(buf), "ADC FAIL");
        }
        render_debug_text(30, buf, C_SUB_R, C_SUB_G, C_SUB_B);
        render_bar(44, pct_from_raw(board->ext_raw[0]));
        render_debug_text(104, "A sample   L/R", C_SUB_R, C_SUB_G, C_SUB_B);
        break;

    /* ── ADC33 ───────────────────────────────────────────── */
    case UI_PAGE_ADC33:
        snprintf(buf, sizeof(buf), "%d%%", pct_from_raw(board->ext_raw[1]));
        render_debug_text(18, buf, C_TEXT_R, C_TEXT_G, C_TEXT_B);
        if (board->adc_ready) {
            snprintf(buf, sizeof(buf), "GPIO33 RAW %04d", board->ext_raw[1]);
        }
        else {
            snprintf(buf, sizeof(buf), "ADC FAIL");
        }
        render_debug_text(30, buf, C_SUB_R, C_SUB_G, C_SUB_B);
        render_bar(44, pct_from_raw(board->ext_raw[1]));
        render_debug_text(104, "A sample   L/R", C_SUB_R, C_SUB_G, C_SUB_B);
        break;

    /* ── SYSTEM ──────────────────────────────────────────── */
    case UI_PAGE_SYSTEM: {
        esp_chip_info_t chip_info;
        esp_chip_info(&chip_info);

        const size_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        const size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);

        render_debug_text(18, "Xiaomiao System Info", C_TITLE_R, C_TITLE_G, C_TITLE_B);

        snprintf(buf, sizeof(buf), "CPU %d MHz x%u  Rev %u",
                 CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
                 (unsigned)chip_info.cores,
                 (unsigned)chip_info.revision);
        render_debug_text(28, buf, C_TEXT_R, C_TEXT_G, C_TEXT_B);

        snprintf(buf, sizeof(buf), "SRAM %u KB  PSRAM %u KB",
                 ui_kb(sram_total), ui_kb(psram_total));
        render_debug_text(38, buf, C_TEXT_R, C_TEXT_G, C_TEXT_B);

        snprintf(buf, sizeof(buf), "Free heap: %u KB", ui_kb(free_heap));
        render_debug_text(48, buf, C_TEXT_R, C_TEXT_G, C_TEXT_B);

        snprintf(buf, sizeof(buf), "Flash %s %s %s",
                 UI_FLASH_MODE, UI_FLASH_FREQ, UI_FLASH_SIZE);
        render_debug_text(58, buf, C_SUB_R, C_SUB_G, C_SUB_B);

        snprintf(buf, sizeof(buf), "IDF %s", esp_get_idf_version());
        render_debug_text(68, buf, C_SUB_R, C_SUB_G, C_SUB_B);

        snprintf(buf, sizeof(buf), "I2C %s  GD %s  MPU %s",
                 board->i2c_ready ? "OK" : "--",
                 board->gd32_present ? "OK" : "--",
                 board->mpu_present ? "OK" : "--");
        render_debug_text(78, buf, C_TEXT_R, C_TEXT_G, C_TEXT_B);

        snprintf(buf, sizeof(buf), "ADC %s  Buz %s  SD %s",
                 board->adc_ready ? "OK" : "--",
                 board->buzzer_ready ? "OK" : "--",
                 board->sd_mounted ? "OK" : "--");
        render_debug_text(88, buf, C_TEXT_R, C_TEXT_G, C_TEXT_B);

        render_bar(98, board->i2c_ready ? 100 : 0);
        render_debug_text(104, "A rescan   L/R", C_SUB_R, C_SUB_G, C_SUB_B);
        break;
    }

    /* ── ABOUT ───────────────────────────────────────────── */
    case UI_PAGE_ABOUT: {
        esp_chip_info_t chip_info;
        esp_chip_info(&chip_info);
        const size_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

        /* Scrollable: show first chunk. B scrolls down (handled in main loop). */
        char details[1200];
        snprintf(details, sizeof(details),
                 "Xiaomiao Handheld  ESP32-WROVER-B  by ZYoung\n\n"
                 "CPU: Xtensa LX6 %d MHz x%u  Rev %u\n"
                 "ESP-IDF: %s\n"
                 "FreeRTOS: %s\n\n"
                 "Flash: %s %s %s\n"
                 "PSRAM: %d MHz\n"
                 "SRAM %u KB  PSRAM %u KB\n\n"
                 "ST7735 160x128 SPI2 RGB565\n"
                 "Keys: 6 active-low\n"
                 "SD: SPI2 CS22\n"
                 "ADC: 36/39/32/33\n"
                 "I2C: GD32 0x40  MPU6050 0x68\n"
                 "PWM: GPIO14 Buzzer  GPIO25/26 EXT\n\n"
                 "WeChat/Tel: 15657325738",
                 CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
                 (unsigned)chip_info.cores,
                 (unsigned)chip_info.revision,
                 esp_get_idf_version(),
                 tskKERNEL_VERSION_NUMBER,
                 UI_FLASH_MODE, UI_FLASH_FREQ, UI_FLASH_SIZE,
                 CONFIG_SPIRAM_SPEED,
                 ui_kb(sram_total), ui_kb(psram_total));

        render_debug_text(18, details, C_TEXT_R, C_TEXT_G, C_TEXT_B);
        render_debug_text(104, "B scroll   L/R", C_SUB_R, C_SUB_G, C_SUB_B);
        break;
    }

    default:
        break;
    }

    /* Status line */
    render_status_line(board);

    /* Action hint */
    render_action_hint(board);
}

/* ============================================================
 *  Action / Cancel / Adjust (mirrors original ui_action etc.)
 * ============================================================ */
static void handle_action(void)
{
    const hw_board_state_t *board = hw_board_state_mut();
    esp_err_t err = ESP_OK;

    switch (s_current_page) {
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
    case UI_PAGE_MOTOR2: {
        const uint8_t motor = (s_current_page == UI_PAGE_MOTOR1) ? 0 : 1;
        hw_board_state_t *b = hw_board_state_mut();
        if (b->motor_running[motor]) {
            hw_gd32_motor_set(motor, b->motor_dir[motor], 0);
            b->motor_running[motor] = false;
            set_action(motor == 0 ? "Motor1 stopped" : "Motor2 stopped");
        }
        else {
            if (b->motor_speed[motor] == 0) {
                set_action("PWM is zero");
                break;
            }
            err = hw_gd32_motor_set(motor, b->motor_dir[motor], b->motor_speed[motor]);
            if (err == ESP_OK) {
                b->motor_running[motor] = true;
                set_action(motor == 0 ? "Motor1 output" : "Motor2 output");
            }
            else {
                set_action("Motor cmd fail");
            }
        }
        break;
    }
    case UI_PAGE_SD:
        hw_sd_try_mount();
        err = board->sd_mounted ? ESP_OK : board->last_sd_err;
        set_action(board->sd_mounted ? "SD mounted" : "No SD card");
        break;
    case UI_PAGE_GPIO25:
    case UI_PAGE_GPIO26: {
        const uint8_t idx = (s_current_page == UI_PAGE_GPIO25) ? 0 : 1;
        hw_board_state_t *b = hw_board_state_mut();
        if (b->ext_out[idx]) {
            err = hw_extio_set(idx, false);
            set_action(err == ESP_OK ? (idx == 0 ? "GPIO25 off" : "GPIO26 off") : "PWM cmd fail");
        }
        else {
            if (b->ext_pwm[idx] == 0) {
                set_action("Duty is zero");
                break;
            }
            err = hw_extio_set(idx, true);
            set_action(err == ESP_OK ? (idx == 0 ? "GPIO25 PWM" : "GPIO26 PWM") : "PWM cmd fail");
        }
        break;
    }
    case UI_PAGE_SYSTEM:
        hw_gd32_probe(true);
        hw_mpu_probe(true);
        err = board->i2c_ready ? ESP_OK : ESP_ERR_INVALID_STATE;
        set_action(board->i2c_ready ? "Rescanned" : "I2C init fail");
        break;
    default:
        break;
    }

    if (err == ESP_OK && s_current_page != UI_PAGE_BUZZER) {
        hw_buzzer_beep(660, 35);
    }
}

static void handle_cancel(void)
{
    hw_board_state_t *board = hw_board_state_mut();

    switch (s_current_page) {
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
        set_action("SD unmounted");
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
            hw_gd32_motor_set(0, board->motor_dir[0], 0);
            board->motor_running[0] = false;
            set_action("Motor1 stopped");
        }
        else {
            board->motor_dir[0] = !board->motor_dir[0];
            set_action("Motor1 dir");
        }
        break;
    case UI_PAGE_MOTOR2:
        if (board->motor_running[1]) {
            hw_gd32_motor_set(1, board->motor_dir[1], 0);
            board->motor_running[1] = false;
            set_action("Motor2 stopped");
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
}

static void handle_adjust(int step)
{
    hw_board_state_t *board = hw_board_state_mut();

    switch (s_current_page) {
    case UI_PAGE_BUZZER: {
        int freq = (int)s_buzzer_freq_hz + step * 110;
        s_buzzer_freq_hz = MAX(440, MIN(freq, 1760));
        set_action("Pitch set");
        break;
    }
    case UI_PAGE_MOTOR1:
    case UI_PAGE_MOTOR2: {
        const uint8_t motor = (s_current_page == UI_PAGE_MOTOR1) ? 0 : 1;
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
        const uint8_t idx = (s_current_page == UI_PAGE_GPIO25) ? 0 : 1;
        int duty = board->ext_pwm[idx] + step * 16;
        board->ext_pwm[idx] = MAX(0, MIN(duty, EXT_PWM_DUTY_MAX));
        if (!board->ext_pwm_ready) {
            set_action("PWM init fail");
        }
        else if (board->ext_out[idx]) {
            esp_err_t err = hw_extio_set(idx, true);
            set_action(err == ESP_OK ? (board->ext_out[idx] ? "Duty set" : "Duty zero") : "PWM cmd fail");
        }
        else {
            set_action("Duty set");
        }
        break;
    }
    default:
        break;
    }
}

/* ============================================================
 *  Public API
 * ============================================================ */

void sdl_demo_create(void)
{
    ESP_LOGI(TAG, "SDL3 demo: creating");

    /* SDL_Init without video — we use a software surface and push pixels
     * to the display via hw_display_flush, bypassing SDL3's ESP-IDF video
     * backend entirely. This avoids the pthread_self() issue and the
     * on_color_trans_done callback conflict. */
    if (!SDL_Init(SDL_INIT_EVENTS)) {
        ESP_LOGE(TAG, "SDL_Init failed: %s", SDL_GetError());
        return;
    }

    /* Create a software surface directly — no window, no video backend.
     * SDL_CreateSoftwareRenderer renders to this surface, and we push
     * the pixels to the display ourselves via hw_display_flush. */
    s_surface = SDL_CreateSurface(160, 128, SDL_PIXELFORMAT_RGB565);
    if (!s_surface) {
        ESP_LOGE(TAG, "SDL_CreateSurface failed: %s", SDL_GetError());
        SDL_Quit();
        return;
    }

    s_renderer = SDL_CreateSoftwareRenderer(s_surface);
    if (!s_renderer) {
        ESP_LOGE(TAG, "SDL_CreateSoftwareRenderer failed: %s", SDL_GetError());
        SDL_DestroySurface(s_surface);
        SDL_Quit();
        return;
    }

    ESP_LOGI(TAG, "SDL3 demo: software renderer -> 160x128 RGB565 surface");
}

void *sdl_demo_task(void *arg)
{
    (void)arg;
    uint32_t last_update_ms = 0;

    /* SDL3 init — must happen inside the pthread task, not in app_main,
     * because SDL_CreateWindow internally calls pthread_self() which
     * requires the ESP-IDF pthread TLS to be set up by pthread_create. */
    sdl_demo_create();

    /* Turn on display (no need to wait for first flush — hw_display_init
     * sets s_lcd_first_flush_done = true immediately since SDL3's video
     * backend handles its own flush-ready callbacks). */
    hw_display_on();

    /* Init history */
    sensor_history_init();

    /* Wait for first display flush to complete, then turn on display */
    for (int i = 0; i < 100 && !hw_display_first_flush_done(); ++i) {
        SDL_Delay(1);
    }
    hw_display_on();

    ESP_LOGI(TAG, "SDL3 demo: task starting main loop");

    while (true) {
        uint32_t now = SDL_GetTicks();
        uint32_t elapsed = now - last_update_ms;

        if (elapsed >= UI_REFRESH_PERIOD_MS) {
            last_update_ms = now;

            /* Update hardware */
            hw_board_process_timers();
            hw_board_update();
            accumulate_adc_to_history();

            /* Inject GPIO button state as SDL events */
            inject_button_events();

            /* Process SDL events */
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                switch (ev.type) {
                case SDL_EVENT_QUIT:
                    ESP_LOGI(TAG, "SDL_QUIT received");
                    goto done;
                case SDL_EVENT_KEY_DOWN:
                    switch (ev.key.scancode) {
                    case SDL_SCANCODE_LEFT:
                        if (s_slide_frames_remaining == 0) {
                            s_slide_target = -UI_SLIDE_PX;
                            s_slide_offset = 0;
                            s_slide_frames_remaining = UI_SLIDE_FRAMES;
                            s_current_page = (s_current_page == 0)
                                ? (ui_page_t)(UI_PAGE_COUNT - 1)
                                : (ui_page_t)(s_current_page - 1);
                        }
                        break;
                    case SDL_SCANCODE_RIGHT:
                        if (s_slide_frames_remaining == 0) {
                            s_slide_target = UI_SLIDE_PX;
                            s_slide_offset = 0;
                            s_slide_frames_remaining = UI_SLIDE_FRAMES;
                            s_current_page = (s_current_page + 1) % UI_PAGE_COUNT;
                        }
                        break;
                    case SDL_SCANCODE_RETURN:
                        handle_action();
                        break;
                    case SDL_SCANCODE_ESCAPE:
                        /* B short press: cancel; held: adjust */
                        handle_cancel();
                        break;
                    case SDL_SCANCODE_UP:
                        handle_adjust(1);
                        break;
                    case SDL_SCANCODE_DOWN:
                        handle_adjust(-1);
                        break;
                    default:
                        break;
                    }
                    break;
                case SDL_EVENT_KEY_UP:
                    break;
                default:
                    break;
                }
            }

            /* Slide animation step */
            if (s_slide_frames_remaining > 0) {
                int step = s_slide_target / UI_SLIDE_FRAMES;
                s_slide_offset += step;
                s_slide_frames_remaining--;
                if (s_slide_frames_remaining == 0) {
                    s_slide_offset = 0;
                }
            }

            /* Render */
            const hw_board_state_t *board = hw_board_state();
            render_page(board);

            /* Present to software surface */
            SDL_RenderPresent(s_renderer);

            /* Push pixels to display via hw_display_flush */
            if (s_surface) {
                hw_display_flush(0, 0, s_surface->w - 1, s_surface->h - 1,
                                 (const uint8_t *)s_surface->pixels);
            }
        }

        /* Frame timing: sleep until next 16ms boundary */
        uint32_t delay = UI_REFRESH_PERIOD_MS - (SDL_GetTicks() - last_update_ms);
        if (delay > UI_REFRESH_PERIOD_MS) delay = UI_REFRESH_PERIOD_MS;
        if (delay > 0) SDL_Delay(delay);
    }

done:
    ESP_LOGI(TAG, "SDL3 demo: exiting");
    SDL_DestroyRenderer(s_renderer);
    SDL_DestroySurface(s_surface);
    SDL_Quit();
    return NULL;
}