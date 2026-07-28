#include "input_api.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define KEY_UP    0
#define KEY_DOWN  1
#define KEY_LEFT  2
#define KEY_RIGHT 3
#define KEY_A     4
#define KEY_B     5

#define BUTTON_ACTIVE_LEVEL 0

static const gpio_num_t s_key_gpios[] = {
    GPIO_NUM_2,   /* UP    */
    GPIO_NUM_13,  /* DOWN  */
    GPIO_NUM_27,  /* LEFT  */
    GPIO_NUM_35,  /* RIGHT */
    GPIO_NUM_34,  /* A     */
    GPIO_NUM_12,  /* B     */
};

static int32_t host_input_get_key(wasm_exec_env_t exec_env, int32_t key_id)
{
    (void)exec_env;
    if (key_id < 0 || key_id >= (int32_t)(sizeof(s_key_gpios) / sizeof(s_key_gpios[0]))) {
        return 0;
    }
    return gpio_get_level(s_key_gpios[key_id]) == BUTTON_ACTIVE_LEVEL ? 1 : 0;
}

static NativeSymbol s_input_symbols[] = {
    { "input_get_key", (void *)host_input_get_key, "(i)i", NULL },
};

void input_api_register(void)
{
    static NativeSymbol symbols[] = {
        { "input_get_key", (void *)host_input_get_key, "(i)i", NULL },
    };
    wasm_runtime_register_natives("xiaomiao", symbols,
                                  sizeof(symbols) / sizeof(NativeSymbol));
    wasm_runtime_register_natives("env", symbols,
                                  sizeof(symbols) / sizeof(NativeSymbol));
    ESP_LOGI("input_api", "registered 1 host function");
}
