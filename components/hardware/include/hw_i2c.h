#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#define PIN_NUM_I2C_SCL   GPIO_NUM_15
#define PIN_NUM_I2C_SDA   GPIO_NUM_21
#define I2C_FREQ_HZ       100000
#define I2C_TIMEOUT_MS    30
#define GD32_ADDR         0x40
#define MPU6050_ADDR      0x68

void hw_i2c_init(void);
i2c_master_bus_handle_t hw_i2c_bus(void);
i2c_master_dev_handle_t hw_i2c_gd32_dev(void);
i2c_master_dev_handle_t hw_i2c_mpu_dev(void);
esp_err_t hw_i2c_write(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len);
esp_err_t hw_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value);
esp_err_t hw_i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, size_t len);
