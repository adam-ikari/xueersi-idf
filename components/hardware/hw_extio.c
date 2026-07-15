#include "hw_extio.h"

#include "hw_buzzer.h"
#include "hw_board.h"

#include "esp_log.h"
#include "driver/ledc.h"

static const char *TAG = "hw_extio";

static ledc_channel_t ext_pwm_channel(uint8_t index)
{
    return index == 0 ? EXT_LEDC_CHANNEL1 : EXT_LEDC_CHANNEL2;
}

void hw_extio_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = BUZZER_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = EXT_LEDC_TIMER,
        .freq_hz = EXT_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Extension PWM timer init failed: %s", esp_err_to_name(err));
        return;
    }

    const ledc_channel_config_t channel_cfg[] = {
        {
            .gpio_num = PIN_NUM_EXT_OUT1,
            .speed_mode = BUZZER_LEDC_MODE,
            .channel = EXT_LEDC_CHANNEL1,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = EXT_LEDC_TIMER,
            .duty = 0,
            .hpoint = 0,
            .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        },
        {
            .gpio_num = PIN_NUM_EXT_OUT2,
            .speed_mode = BUZZER_LEDC_MODE,
            .channel = EXT_LEDC_CHANNEL2,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = EXT_LEDC_TIMER,
            .duty = 0,
            .hpoint = 0,
            .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        },
    };

    err = ledc_channel_config(&channel_cfg[0]);
    if (err == ESP_OK) {
        err = ledc_channel_config(&channel_cfg[1]);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Extension PWM channel init failed: %s", esp_err_to_name(err));
        return;
    }

    hw_board_state_t *board = hw_board_state_mut();
    board->ext_pwm_ready = true;
    (void)hw_extio_set(0, false);
    (void)hw_extio_set(1, false);
}

esp_err_t hw_extio_set(uint8_t index, bool on)
{
    hw_board_state_t *board = hw_board_state_mut();
    if (index >= 2 || !board->ext_pwm_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    const ledc_channel_t channel = ext_pwm_channel(index);
    const uint32_t duty = on ? board->ext_pwm[index] : 0;

    esp_err_t err = ledc_set_duty(BUZZER_LEDC_MODE, channel, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(BUZZER_LEDC_MODE, channel);
    }
    if (err == ESP_OK) {
        board->ext_out[index] = on && duty > 0;
    }
    return err;
}

esp_err_t hw_extio_set_pwm(uint8_t index, uint8_t duty)
{
    hw_board_state_t *board = hw_board_state_mut();
    if (index >= 2 || !board->ext_pwm_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    board->ext_pwm[index] = duty;
    return ESP_OK;
}
