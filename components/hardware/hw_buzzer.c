#include "hw_buzzer.h"

#include "hw_board.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/ledc.h"

static const char *TAG = "hw_buzzer";

static uint32_t s_buzzer_stop_at;
static uint32_t s_buzzer_freq_hz = 988;

void hw_buzzer_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = BUZZER_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = BUZZER_LEDC_TIMER,
        .freq_hz = 880,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Buzzer timer init failed: %s", esp_err_to_name(err));
        return;
    }

    ledc_channel_config_t channel_cfg = {
        .gpio_num = PIN_NUM_BUZZER,
        .speed_mode = BUZZER_LEDC_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BUZZER_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
    };
    err = ledc_channel_config(&channel_cfg);
    if (err == ESP_OK) {
        hw_board_state_t *board = hw_board_state_mut();
        board->buzzer_ready = true;
    } else {
        ESP_LOGW(TAG, "Buzzer channel init failed: %s", esp_err_to_name(err));
    }
}

void hw_buzzer_stop(void)
{
    hw_board_state_t *board = hw_board_state_mut();
    if (!board->buzzer_ready) {
        return;
    }
    ledc_stop(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    s_buzzer_stop_at = 0;
}

void hw_buzzer_beep(uint32_t freq_hz, uint32_t ms)
{
    hw_board_state_t *board = hw_board_state_mut();
    if (!board->buzzer_ready) {
        return;
    }
    esp_err_t err = ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, freq_hz);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Buzzer frequency %lu Hz failed: %s", (unsigned long)freq_hz, esp_err_to_name(err));
        return;
    }
    err = ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, BUZZER_DUTY);
    if (err == ESP_OK) {
        err = ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Buzzer duty update failed: %s", esp_err_to_name(err));
        return;
    }
    s_buzzer_stop_at = (uint32_t)(esp_timer_get_time() / 1000) + ms;
    s_buzzer_freq_hz = freq_hz;
}

void hw_buzzer_timer(void)
{
    if (s_buzzer_stop_at) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if ((int32_t)(now - s_buzzer_stop_at) >= 0) {
            hw_buzzer_stop();
        }
    }
}
