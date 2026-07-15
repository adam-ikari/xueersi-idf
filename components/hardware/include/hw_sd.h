#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define PIN_NUM_SD_CS               GPIO_NUM_22
#define SD_SPI_MAX_FREQ_KHZ         10000

void hw_sd_try_mount(void);
esp_err_t hw_sd_unmount(void);
void hw_sd_info(char *name, size_t name_size, uint32_t *mb);
