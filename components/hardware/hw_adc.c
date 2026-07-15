#include "hw_adc.h"

#include "hw_board.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include <sys/param.h>

static const char *TAG = "hw_adc";

static adc_oneshot_unit_handle_t s_adc_handle;

int pct_from_raw(int raw)
{
    raw = MAX(0, MIN(raw, ADC_RAW_MAX));
    return (raw * 100) / ADC_RAW_MAX;
}

static esp_err_t adc_read_one(adc_channel_t channel, int *raw)
{
    esp_err_t err = adc_oneshot_read(s_adc_handle, channel, raw);
    hw_board_state_t *board = hw_board_state_mut();
    if (err != ESP_OK && board->last_adc_err == ESP_OK) {
        board->last_adc_err = err;
    }
    return err;
}

void hw_adc_init(void)
{
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&init_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        hw_board_state_t *board = hw_board_state_mut();
        board->last_adc_err = err;
        ESP_LOGW(TAG, "ADC init failed: %s", esp_err_to_name(err));
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(s_adc_handle, ADC_LIGHT_CHAN, &chan_cfg);
    if (err == ESP_OK) {
        err = adc_oneshot_config_channel(s_adc_handle, ADC_TEMP_CHAN, &chan_cfg);
    }
    if (err == ESP_OK) {
        err = adc_oneshot_config_channel(s_adc_handle, ADC_EXT_IN1_CHAN, &chan_cfg);
    }
    if (err == ESP_OK) {
        err = adc_oneshot_config_channel(s_adc_handle, ADC_EXT_IN2_CHAN, &chan_cfg);
    }
    if (err != ESP_OK) {
        hw_board_state_t *board = hw_board_state_mut();
        board->last_adc_err = err;
        ESP_LOGW(TAG, "ADC channel config failed: %s", esp_err_to_name(err));
        return;
    }

    hw_board_state_t *board = hw_board_state_mut();
    board->last_adc_err = ESP_OK;
    board->adc_ready = true;
}

int hw_adc_read_light(void)
{
    int raw = 0;
    if (adc_read_one(ADC_LIGHT_CHAN, &raw) != ESP_OK) {
        return 0;
    }
    return raw;
}

int hw_adc_read_temp(void)
{
    int raw = 0;
    if (adc_read_one(ADC_TEMP_CHAN, &raw) != ESP_OK) {
        return 0;
    }
    return raw;
}

int hw_adc_read_ext(uint8_t idx)
{
    adc_channel_t ch = (idx == 0) ? ADC_EXT_IN1_CHAN : ADC_EXT_IN2_CHAN;
    int raw = 0;
    if (adc_read_one(ch, &raw) != ESP_OK) {
        return 0;
    }
    return raw;
}

void hw_adc_update(void)
{
    hw_board_state_t *board = hw_board_state_mut();

    if (!board->adc_ready) {
        if (board->last_adc_err == ESP_OK) {
            board->last_adc_err = ESP_ERR_INVALID_STATE;
        }
        return;
    }

    if (!s_adc_handle) {
        board->last_adc_err = ESP_ERR_INVALID_STATE;
        return;
    }

    int raw = 0;
    board->last_adc_err = ESP_OK;

    if (adc_read_one(ADC_LIGHT_CHAN, &raw) == ESP_OK) {
        board->light_raw = raw;
    }
    if (adc_read_one(ADC_TEMP_CHAN, &raw) == ESP_OK) {
        board->temp_raw = raw;
    }
    if (adc_read_one(ADC_EXT_IN1_CHAN, &raw) == ESP_OK) {
        board->ext_raw[0] = raw;
    }
    if (adc_read_one(ADC_EXT_IN2_CHAN, &raw) == ESP_OK) {
        board->ext_raw[1] = raw;
    }
}
