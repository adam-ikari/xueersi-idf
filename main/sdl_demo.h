#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ── Page enum (matches original LVGL dashboard) ──────────── */
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

void sdl_demo_create(void);
void sdl_demo_task(void *arg);