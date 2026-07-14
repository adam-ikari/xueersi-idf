#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define GD32_LED1_REG       0xA0
#define GD32_LED2_REG       0xA1
#define GD32_MOTOR1_REG     0x0E
#define GD32_MOTOR2_REG     0x06
#define GD32_REPROBE_PERIOD_MS 1500

void hw_gd32_probe(bool force);
bool hw_gd32_present(void);
esp_err_t hw_gd32_set_led(uint8_t idx, bool on);
esp_err_t hw_gd32_motor_set(uint8_t motor, bool dir, uint8_t speed);
esp_err_t hw_gd32_motor_stop_all(void);
