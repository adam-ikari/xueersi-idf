/**
 * wasm3 WebAssembly game engine for Xiaomiao ESP32.
 *
 * Runs wasm bytecode on core 0 via wasm3 interpreter. The wasm module
 * calls render_submit_cube() / physics_step() host functions. Core 1
 * drains the render queue each frame.
 */
#include "wasm3.h"
#include "m3_env.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "render_queue.h"
#include "tinygl_physics.h"

#include "wasm_game.wasm.h"

static const char *TAG = "wasm3";
static IM3Runtime s_runtime = NULL;

/* ── Host function implementations ─────────────────────────────────────── */

static m3ApiRawFunction(host_render_submit_cube)
{
    m3ApiGetArg(float, x);
    m3ApiGetArg(float, y);
    m3ApiGetArg(float, z);
    m3ApiGetArg(float, size);
    m3ApiGetArg(float, rx);
    m3ApiGetArg(float, ry);
    m3ApiGetArg(int,   tex_id);

    if (tex_id < 1 || tex_id > 4) tex_id = 1;
    render_queue_push((render_cmd_t){
        .x = x, .y = y, .z = z,
        .size = size,
        .rx = rx, .ry = ry,
        .tex_id = (unsigned int)tex_id,
    });
    m3ApiSuccess();
}

static m3ApiRawFunction(host_physics_step)
{
    m3ApiGetArg(float, dt);
    physics_step(dt);
    m3ApiSuccess();
}

static m3ApiRawFunction(host_physics_body_count)
{
    m3ApiReturnType(int);
    int count = 0;
    for (int i = 0; i < MAX_BODIES; i++)
        if (s_bodies[i].active) count++;
    m3ApiReturn(count);
}

static m3ApiRawFunction(host_physics_spawn)
{
    m3ApiGetArg(float, x);
    m3ApiGetArg(float, y);
    m3ApiGetArg(float, z);
    m3ApiGetArg(float, size);
    m3ApiReturnType(int);
    m3ApiReturn(physics_spawn(x, y, z, size));
}

static m3ApiRawFunction(host_physics_body_pos)
{
    m3ApiGetArg(int, idx);
    m3ApiGetArg(int, x_ptr);
    m3ApiGetArg(int, y_ptr);
    m3ApiGetArg(int, z_ptr);

    body_t *b = physics_get(idx);
    if (b && b->active && s_runtime) {
        uint32_t mem_size = 0;
        uint8_t *mem = m3_GetMemory(s_runtime, &mem_size, 0);
        if (mem && (uint32_t)x_ptr + 4 <= mem_size) *(float *)(mem + x_ptr) = b->x;
        if (mem && (uint32_t)y_ptr + 4 <= mem_size) *(float *)(mem + y_ptr) = b->y;
        if (mem && (uint32_t)z_ptr + 4 <= mem_size) *(float *)(mem + z_ptr) = b->z;
    }
    m3ApiSuccess();
}

/* ── Link host functions ───────────────────────────────────────────────── */

static bool link_host_functions(IM3Module module)
{
    #define LINK(name, func, sig) \
        m3_LinkRawFunction(module, "env", name, sig, func)

    LINK("render_submit_cube",    host_render_submit_cube,    "F(FFFFFii)");
    LINK("physics_step",          host_physics_step,          "v(F)");
    LINK("physics_body_count",    host_physics_body_count,    "i()");
    LINK("physics_spawn",         host_physics_spawn,         "i(FFFF)");
    LINK("physics_body_pos",      host_physics_body_pos,      "v(iiii)");

    ESP_LOGI(TAG, "Host functions linked");
    return true;
}

/* ── WASM3 game task (core 0) ──────────────────────────────────────────── */

void wasm3_game_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "wasm3 game task starting on core %d", xPortGetCoreID());

    IM3Environment env = m3_NewEnvironment();
    if (!env) { ESP_LOGE(TAG, "m3_NewEnvironment failed"); return; }

    s_runtime = m3_NewRuntime(env, 65536, NULL);
    if (!s_runtime) { ESP_LOGE(TAG, "m3_NewRuntime failed"); return; }

    IM3Module module;
    M3Result result = m3_ParseModule(env, &module, wasm_game_wasm, wasm_game_wasm_len);
    if (result) { ESP_LOGE(TAG, "Parse: %s", result); return; }
    ESP_LOGI(TAG, "WASM parsed (%u bytes)", wasm_game_wasm_len);

    result = m3_LoadModule(s_runtime, module);
    if (result) { ESP_LOGE(TAG, "Load: %s", result); return; }

    if (!link_host_functions(module)) { ESP_LOGE(TAG, "Link failed"); return; }

    IM3Function fn;
    result = m3_FindFunction(&fn, s_runtime, "game_init");
    if (!result) { m3_CallV(fn); ESP_LOGI(TAG, "game_init() done"); }

    result = m3_FindFunction(&fn, s_runtime, "game_update");
    if (result) { ESP_LOGE(TAG, "game_update: %s", result); return; }

    ESP_LOGI(TAG, "Game loop on core 0");
    int64_t last_us = esp_timer_get_time();
    int n = 0;
    while (1) {
        M3Result cr = m3_CallV(fn);
        n++;
        if (cr) {
            ESP_LOGE(TAG, "iter %d: %s", n, cr);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if ((n % 30) == 0) ESP_LOGI(TAG, "%d iters OK", n);

        int64_t now = esp_timer_get_time();
        int64_t el = now - last_us;
        last_us = now;
        vTaskDelay(pdMS_TO_TICKS(el < 33333 ? (33333 - el) / 1000 : 1));
    }
}