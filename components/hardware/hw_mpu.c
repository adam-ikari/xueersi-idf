#include "hw_mpu.h"

#include "hw_i2c.h"
#include "hw_board.h"

#include "esp_timer.h"

#include <math.h>
#include <string.h>

static uint32_t s_last_mpu_probe_ms;
static bool s_mpu_probe_seen;

static int16_t i16_be(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] << 8 | p[1]);
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", src ? src : "");
}

void hw_mpu_probe(bool force)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (!force && s_mpu_probe_seen && now_ms - s_last_mpu_probe_ms < MPU_REPROBE_PERIOD_MS) {
        return;
    }
    s_mpu_probe_seen = true;
    s_last_mpu_probe_ms = now_ms;

    hw_board_state_t *board = hw_board_state_mut();
    board->mpu_present = false;
    board->mpu_whoami = 0;
    copy_text(board->gesture, sizeof(board->gesture), "ABSENT");

    i2c_master_bus_handle_t bus = hw_i2c_bus();
    i2c_master_dev_handle_t dev = hw_i2c_mpu_dev();

    if (!bus || !dev) {
        board->last_mpu_err = ESP_ERR_INVALID_STATE;
        return;
    }

    esp_err_t err = i2c_master_probe(bus, MPU6050_ADDR, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        board->last_mpu_err = err;
        return;
    }

    uint8_t who = 0;
    err = hw_i2c_read_reg(dev, MPU6050_REG_WHO_AM_I, &who, 1);
    if (err != ESP_OK) {
        board->last_mpu_err = err;
        return;
    }

    board->mpu_whoami = who;
    if (who != MPU6050_WHO_AM_I_VALUE) {
        board->last_mpu_err = ESP_ERR_INVALID_ARG;
        return;
    }

    err = hw_i2c_write_reg(dev, MPU6050_REG_PWR_MGMT_1, 0x00);
    if (err != ESP_OK) {
        board->last_mpu_err = err;
        return;
    }

    board->mpu_present = true;
    board->last_mpu_err = ESP_OK;
    copy_text(board->gesture, sizeof(board->gesture), "READY");
}

void hw_mpu_read(void)
{
    hw_board_state_t *board = hw_board_state_mut();

    if (!board->mpu_present) {
        hw_mpu_probe(false);
        return;
    }

    uint8_t data[14] = {0};
    esp_err_t err = hw_i2c_read_reg(hw_i2c_mpu_dev(), MPU6050_REG_ACCEL_XOUT_H, data, sizeof(data));
    board->last_mpu_err = err;
    if (err != ESP_OK) {
        board->mpu_present = false;
        copy_text(board->gesture, sizeof(board->gesture), "ABSENT");
        return;
    }

    board->acc[0] = i16_be(&data[0]);
    board->acc[1] = i16_be(&data[2]);
    board->acc[2] = i16_be(&data[4]);
    board->gyro[0] = i16_be(&data[8]);
    board->gyro[1] = i16_be(&data[10]);
    board->gyro[2] = i16_be(&data[12]);

    const float ax = board->acc[0] / 16384.0f;
    const float ay = board->acc[1] / 16384.0f;
    const float az = board->acc[2] / 16384.0f;
    board->pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * 57.29578f;
    board->roll  = atan2f(ay, az) * 57.29578f;

    if (board->pitch > 25.0f) {
        copy_text(board->gesture, sizeof(board->gesture), "TILT UP");
    } else if (board->pitch < -25.0f) {
        copy_text(board->gesture, sizeof(board->gesture), "TILT DN");
    } else if (board->roll > 25.0f) {
        copy_text(board->gesture, sizeof(board->gesture), "TILT R");
    } else if (board->roll < -25.0f) {
        copy_text(board->gesture, sizeof(board->gesture), "TILT L");
    } else {
        copy_text(board->gesture, sizeof(board->gesture), "LEVEL");
    }
}

bool hw_mpu_present(void)
{
    return hw_board_state_mut()->mpu_present;
}
