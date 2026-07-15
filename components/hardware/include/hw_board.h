#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool i2c_ready;
    bool gd32_present;
    bool mpu_present;
    bool sd_mounted;
    bool buzzer_ready;
    bool adc_ready;
    bool ext_pwm_ready;
    bool led1_on;
    bool led2_on;
    bool motor_running[2];
    bool motor_dir[2];
    bool ext_out[2];
    uint8_t motor_speed[2];
    uint8_t ext_pwm[2];
    uint8_t mpu_whoami;
    uint32_t samples;
    int light_raw;
    int temp_raw;
    int ext_raw[2];
    int16_t acc[3];
    int16_t gyro[3];
    float pitch;
    float roll;
    char gesture[12];
    char sd_name[24];
    uint32_t sd_mb;
    esp_err_t last_adc_err;
    esp_err_t last_gd32_err;
    esp_err_t last_mpu_err;
    esp_err_t last_sd_err;
    char action[32];
} hw_board_state_t;

void hw_board_init(void);
void hw_board_update(void);
void hw_board_process_timers(void);
const hw_board_state_t *hw_board_state(void);

/* Internal-only mutable access for sibling modules (e.g. hw_gd32) */
hw_board_state_t *hw_board_state_mut(void);
