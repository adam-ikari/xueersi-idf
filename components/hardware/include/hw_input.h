#pragma once

#include "driver/gpio.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BUTTON_ACTIVE_LEVEL  0
#define BUTTON_DEBOUNCE_MS   25

#define LV_KEY_UP     17
#define LV_KEY_DOWN   18
#define LV_KEY_LEFT   19
#define LV_KEY_RIGHT  20
#define LV_KEY_ENTER  10
#define LV_KEY_ESC    27

typedef struct {
    gpio_num_t gpio;
    uint32_t key;       // LV_KEY_* code (for SDL3 remapping later)
    const char *name;
} hw_button_t;

void hw_input_init(void);
const hw_button_t *hw_input_buttons(size_t *count);
bool hw_input_is_pressed(size_t idx);
void hw_input_poll(void);  // empty for now — P4 implements SDL event injection
