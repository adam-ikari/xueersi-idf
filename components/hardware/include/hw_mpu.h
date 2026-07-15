#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define MPU6050_REG_ACCEL_XOUT_H  0x3B
#define MPU6050_REG_PWR_MGMT_1    0x6B
#define MPU6050_REG_WHO_AM_I      0x75
#define MPU6050_WHO_AM_I_VALUE    0x68
#define MPU_REPROBE_PERIOD_MS     1500

void hw_mpu_probe(bool force);
void hw_mpu_read(void);
bool hw_mpu_present(void);
