/*
 * hw_board.c — Top-level hardware board init/update/timer dispatch.
 *
 * This module owns the single `s_board` state struct and calls each
 * sub-module's init / probe / update / timer functions in the correct
 * order.  It is the only hardware module that includes all the others.
 *
 * SPDX-FileCopyrightText: 2024 Xueersi Xiaomiao Handheld Project
 * SPDX-License-Identifier: MIT
 */

#include "hw_board.h"

#include "hw_i2c.h"
#include "hw_display.h"
#include "hw_input.h"
#include "hw_adc.h"
#include "hw_buzzer.h"
#include "hw_extio.h"
#include "hw_gd32.h"
#include "hw_mpu.h"

#include "esp_log.h"

static const char *TAG = "hw_board";

/* ── Global board state ──────────────────────────────────── */

static hw_board_state_t s_board = {
    .gesture     = "ABSENT",
    .sd_name     = "NO CARD",
    .motor_speed = {120, 120},
    .ext_pwm     = {128, 128},
    .last_sd_err = ESP_ERR_NOT_FOUND,
    .action      = "Ready",
};

/* ── Internal: probe all I2C devices ─────────────────────── */

static void i2c_probe_devices(bool force)
{
    if (!hw_i2c_bus()) {
        hw_board_state_t *b = hw_board_state_mut();
        b->i2c_ready      = false;
        b->gd32_present   = false;
        b->mpu_present    = false;
        b->last_gd32_err  = ESP_ERR_INVALID_STATE;
        b->last_mpu_err   = ESP_ERR_INVALID_STATE;
        return;
    }

    hw_board_state_t *b = hw_board_state_mut();
    b->i2c_ready = true;

    hw_gd32_probe(force);

    if (force || !b->mpu_present) {
        hw_mpu_probe(force);
    }
}

/* ── Public API ──────────────────────────────────────────── */

const hw_board_state_t *hw_board_state(void)
{
    return &s_board;
}

hw_board_state_t *hw_board_state_mut(void)
{
    return &s_board;
}

void hw_board_init(void)
{
    ESP_LOGI(TAG, "Hardware board init starting");

    hw_i2c_init();
    hw_display_init();
    hw_input_init();
    hw_adc_init();
    hw_buzzer_init();
    hw_extio_init();

    /* SD card is mounted on demand by the UI layer — not here. */

    /* Do the initial I2C device probe (GD32 + MPU6050). */
    i2c_probe_devices(true);

    /* Stop any motors that might be running from a previous firmware. */
    if (s_board.gd32_present) {
        hw_gd32_motor_stop_all();
    }

    ESP_LOGI(TAG, "Hardware board init complete");
}

void hw_board_update(void)
{
    /* Lazy re-probe of missing I2C devices (rate-limited inside hw_gd32/hw_mpu). */
    i2c_probe_devices(false);

    /* Read ADC channels (light, thermistor, external inputs). */
    hw_adc_update();

    /* Read MPU6050 accelerometer / gyroscope if present. */
    if (s_board.mpu_present) {
        hw_mpu_read();
    }

    s_board.samples++;
}

void hw_board_process_timers(void)
{
    hw_buzzer_timer();
}