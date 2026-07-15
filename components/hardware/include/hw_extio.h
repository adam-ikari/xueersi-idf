#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define PIN_NUM_EXT_OUT1            GPIO_NUM_25
#define PIN_NUM_EXT_OUT2            GPIO_NUM_26
#define PIN_NUM_EXT_IN1             GPIO_NUM_32
#define PIN_NUM_EXT_IN2             GPIO_NUM_33
#define EXT_LEDC_TIMER              LEDC_TIMER_1
#define EXT_LEDC_CHANNEL1           LEDC_CHANNEL_1
#define EXT_LEDC_CHANNEL2           LEDC_CHANNEL_2
#define EXT_PWM_FREQ_HZ             1000
#define EXT_PWM_DUTY_MAX            255

void hw_extio_init(void);
esp_err_t hw_extio_set(uint8_t index, bool on);
esp_err_t hw_extio_set_pwm(uint8_t index, uint8_t duty);
