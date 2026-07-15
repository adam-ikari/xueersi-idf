#pragma once

#include <stdint.h>
#include <stdbool.h>

#define PIN_NUM_BUZZER              GPIO_NUM_14
#define BUZZER_LEDC_MODE            LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_TIMER           LEDC_TIMER_0
#define BUZZER_LEDC_CHANNEL         LEDC_CHANNEL_0
#define BUZZER_DUTY                 128

void hw_buzzer_init(void);
void hw_buzzer_beep(uint32_t freq_hz, uint32_t ms);
void hw_buzzer_stop(void);
void hw_buzzer_timer(void);
