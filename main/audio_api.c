#include "audio_api.h"
#include "hw_buzzer.h"
#include "esp_log.h"
#include "wasm_export.h"

static int32_t host_audio_play_tone(wasm_exec_env_t exec_env, int32_t freq, int32_t duration_ms)
{
    (void)exec_env;
    hw_buzzer_beep((uint32_t)freq, (uint32_t)duration_ms);
    return 0;
}

static int32_t host_audio_stop(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    hw_buzzer_stop();
    return 0;
}

void audio_api_register(void)
{
    /* Signatures match WASM imports: no return value (WASM declares void). */
    static NativeSymbol s_audio_symbols[] = {
        { "audio_play_tone", (void *)host_audio_play_tone, "(ii)", NULL },
        { "audio_stop",      (void *)host_audio_stop,      "()",   NULL },
    };
    wasm_runtime_register_natives("xiaomiao",
                                s_audio_symbols,
                                sizeof(s_audio_symbols) / sizeof(NativeSymbol));
    wasm_runtime_register_natives("env",
                                s_audio_symbols,
                                sizeof(s_audio_symbols) / sizeof(NativeSymbol));
    ESP_LOGI("audio_api", "registered 2 host functions");
}
