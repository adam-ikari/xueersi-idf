#pragma once

#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define ADC_LIGHT_CHAN    ADC_CHANNEL_0
#define ADC_TEMP_CHAN     ADC_CHANNEL_3
#define ADC_EXT_IN1_CHAN  ADC_CHANNEL_4
#define ADC_EXT_IN2_CHAN  ADC_CHANNEL_5
#define ADC_RAW_MAX       4095

int pct_from_raw(int raw);

void hw_adc_init(void);
int hw_adc_read_light(void);
int hw_adc_read_temp(void);
int hw_adc_read_ext(uint8_t idx);
void hw_adc_update(void);
