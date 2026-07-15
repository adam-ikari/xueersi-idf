#include "hw_i2c.h"

#include "esp_log.h"

static const char *TAG = "hw_i2c";

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_gd32_dev;
static i2c_master_dev_handle_t s_mpu_dev;

i2c_master_bus_handle_t hw_i2c_bus(void)
{
    return s_i2c_bus;
}

i2c_master_dev_handle_t hw_i2c_gd32_dev(void)
{
    return s_gd32_dev;
}

i2c_master_dev_handle_t hw_i2c_mpu_dev(void)
{
    return s_mpu_dev;
}

esp_err_t hw_i2c_write(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len)
{
    if (!dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit(dev, data, len, I2C_TIMEOUT_MS);
}

esp_err_t hw_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return hw_i2c_write(dev, data, sizeof(data));
}

esp_err_t hw_i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, size_t len)
{
    if (!dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, I2C_TIMEOUT_MS);
}

void hw_i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_NUM_I2C_SDA,
        .scl_io_num = PIN_NUM_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C init failed: %s", esp_err_to_name(err));
        return;
    }

    const i2c_device_config_t gd32_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = GD32_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    const i2c_device_config_t mpu_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MPU6050_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };

    err = i2c_master_bus_add_device(s_i2c_bus, &gd32_cfg, &s_gd32_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GD32 I2C device add failed: %s", esp_err_to_name(err));
        s_gd32_dev = NULL;
    }

    err = i2c_master_bus_add_device(s_i2c_bus, &mpu_cfg, &s_mpu_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MPU I2C device add failed: %s", esp_err_to_name(err));
        s_mpu_dev = NULL;
    }
}
