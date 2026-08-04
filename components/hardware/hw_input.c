#include "hw_input.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"

static const char *TAG = "hw_input";

static const hw_button_t s_buttons[] = {
    {GPIO_NUM_2,  LV_KEY_UP,    "UP"},
    {GPIO_NUM_13, LV_KEY_DOWN,  "DOWN"},
    {GPIO_NUM_27, LV_KEY_LEFT,  "LEFT"},
    {GPIO_NUM_35, LV_KEY_RIGHT, "RIGHT"},
    {GPIO_NUM_34, LV_KEY_ENTER, "A"},
    {GPIO_NUM_12, LV_KEY_ESC,   "B"},
};

static uint32_t s_last_change_ms[sizeof(s_buttons) / sizeof(s_buttons[0])];
static int      s_last_raw[sizeof(s_buttons) / sizeof(s_buttons[0])];
static int      s_stable[sizeof(s_buttons) / sizeof(s_buttons[0])];
static bool     s_was_pressed[HW_KEY_COUNT];  /* last known stable state for edge detect */

void hw_input_init(void)
{
    uint64_t pin_mask = 0;
    uint64_t pullup_mask = 0;

    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        pin_mask |= 1ULL << s_buttons[i].gpio;
        if (s_buttons[i].gpio != GPIO_NUM_34 && s_buttons[i].gpio != GPIO_NUM_35) {
            pullup_mask |= 1ULL << s_buttons[i].gpio;
        }
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_config_t pullup_conf = {
        .pin_bit_mask = pullup_mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pullup_conf));

    /* initialise debounce state */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    size_t n = sizeof(s_buttons) / sizeof(s_buttons[0]);
    for (size_t i = 0; i < n; ++i) {
        s_last_change_ms[i] = now_ms;
        s_last_raw[i] = -1;
        s_stable[i]   = -1;
        s_was_pressed[i] = false;
    }

    ESP_LOGI(TAG, "buttons init ok (%u buttons)", (unsigned)n);
}

const hw_button_t *hw_input_buttons(size_t *count)
{
    if (count) {
        *count = sizeof(s_buttons) / sizeof(s_buttons[0]);
    }
    return s_buttons;
}

bool hw_input_is_pressed(size_t idx)
{
    size_t n = sizeof(s_buttons) / sizeof(s_buttons[0]);
    if (idx >= n) {
        return false;
    }

    int raw = (gpio_get_level(s_buttons[idx].gpio) == BUTTON_ACTIVE_LEVEL) ? (int)idx : -1;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    if (raw != s_last_raw[idx]) {
        s_last_raw[idx] = raw;
        s_last_change_ms[idx] = now_ms;
        if (raw < 0) {
            s_stable[idx] = -1;
        }
    }

    uint32_t elapsed = now_ms - s_last_change_ms[idx];
    if (elapsed >= BUTTON_DEBOUNCE_MS) {
        s_stable[idx] = s_last_raw[idx];
    }

    return s_stable[idx] >= 0;
}

void hw_input_poll(void)
{
    /* Edge detection: compare current stable state to last-known state,
     * emit events for any transitions. */
    hw_key_event_t buf[6];
    hw_input_poll_events(buf, 6);
}

int hw_input_poll_events(hw_key_event_t *events, int max)
{
    int count = 0;
    for (int i = 0; i < HW_KEY_COUNT && count < max; i++) {
        bool now = hw_input_is_pressed(i);
        if (now != s_was_pressed[i]) {
            s_was_pressed[i] = now;
            events[count].btn_idx = (uint8_t)i;
            events[count].pressed = now;
            count++;
        }
    }
    return count;
}
