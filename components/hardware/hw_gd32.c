#include "hw_gd32.h"

#include "hw_i2c.h"
#include "hw_board.h"

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "hw_gd32";

static uint32_t s_last_gd32_probe_ms;
static bool s_gd32_probe_seen;

static void hw_gd32_mark_absent(void)
{
    hw_board_state_t *board = hw_board_state_mut();
    board->gd32_present = false;
    board->led1_on = false;
    board->led2_on = false;
    board->motor_running[0] = false;
    board->motor_running[1] = false;
}

static esp_err_t hw_gd32_write_reg(uint8_t reg, uint8_t value)
{
    hw_board_state_t *board = hw_board_state_mut();
    esp_err_t err = hw_i2c_write_reg(hw_i2c_gd32_dev(), reg, value);
    board->last_gd32_err = err;
    if (err == ESP_OK) {
        board->gd32_present = true;
    } else {
        hw_gd32_mark_absent();
    }
    return err;
}

esp_err_t hw_gd32_motor_stop_all(void)
{
    const uint8_t data[] = {0x00, 0x00, 0x00, 0x00, 0x00};
    hw_board_state_t *board = hw_board_state_mut();
    esp_err_t err = hw_i2c_write(hw_i2c_gd32_dev(), data, sizeof(data));
    board->last_gd32_err = err;
    if (err == ESP_OK) {
        board->gd32_present = true;
        board->motor_running[0] = false;
        board->motor_running[1] = false;
    } else {
        hw_gd32_mark_absent();
    }
    return err;
}

esp_err_t hw_gd32_motor_set(uint8_t motor, bool dir, uint8_t speed)
{
    const uint8_t reg = (motor == 0) ? GD32_MOTOR1_REG : GD32_MOTOR2_REG;
    const uint16_t pwm = ((uint16_t)speed) << 4;
    const uint8_t pwm_l = pwm & 0xFF;
    const uint8_t pwm_h = pwm >> 8;
    uint8_t data[9] = {reg, 0, 0, 0, 0, 0, 0, 0, 0};

    if (dir) {
        data[3] = pwm_l;
        data[4] = pwm_h;
    } else {
        data[7] = pwm_l;
        data[8] = pwm_h;
    }

    hw_board_state_t *board = hw_board_state_mut();
    esp_err_t err = hw_i2c_write(hw_i2c_gd32_dev(), data, sizeof(data));
    board->last_gd32_err = err;
    if (err == ESP_OK) {
        board->gd32_present = true;
        board->motor_running[motor] = speed > 0;
    } else {
        hw_gd32_mark_absent();
    }
    return err;
}

void hw_gd32_probe(bool force)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (!force && s_gd32_probe_seen && now_ms - s_last_gd32_probe_ms < GD32_REPROBE_PERIOD_MS) {
        return;
    }
    s_gd32_probe_seen = true;
    s_last_gd32_probe_ms = now_ms;

    hw_board_state_t *board = hw_board_state_mut();

    if (!hw_i2c_bus() || !hw_i2c_gd32_dev()) {
        board->last_gd32_err = ESP_ERR_INVALID_STATE;
        board->gd32_present = false;
    } else {
        board->last_gd32_err = i2c_master_probe(hw_i2c_bus(), GD32_ADDR, I2C_TIMEOUT_MS);
        board->gd32_present = (board->last_gd32_err == ESP_OK);
    }

    if (!board->gd32_present) {
        hw_gd32_mark_absent();
    }
}

bool hw_gd32_present(void)
{
    return hw_board_state()->gd32_present;
}

esp_err_t hw_gd32_set_led(uint8_t idx, bool on)
{
    uint8_t reg = (idx == 0) ? GD32_LED1_REG : GD32_LED2_REG;
    esp_err_t err = hw_gd32_write_reg(reg, on ? 1 : 0);
    if (err == ESP_OK) {
        hw_board_state_t *board = hw_board_state_mut();
        if (idx == 0) {
            board->led1_on = on;
        } else {
            board->led2_on = on;
        }
    }
    return err;
}
