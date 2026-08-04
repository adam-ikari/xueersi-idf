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

#define HW_KEY_COUNT 6

typedef struct {
    gpio_num_t gpio;
    uint32_t key;       // LV_KEY_* code (for SDL3 remapping later)
    const char *name;
} hw_button_t;

/** Single key transition event (down or up edge). */
typedef struct {
    uint8_t btn_idx;    /* 0..5, mapping: 0=UP 1=DOWN 2=LEFT 3=RIGHT 4=A 5=B */
    bool    pressed;    /* true = down edge, false = up edge */
} hw_key_event_t;

void hw_input_init(void);
const hw_button_t *hw_input_buttons(size_t *count);
bool hw_input_is_pressed(size_t idx);

/** Poll all buttons and collect edge events since last call.
 *  @param events  Output buffer, caller-allocated.
 *  @param max     Capacity of |events|.
 *  @return Number of events written (0..max). */
int hw_input_poll_events(hw_key_event_t *events, int max);
