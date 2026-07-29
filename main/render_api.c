/**
 * TinyGL Render Host API for WAMR wasm games.
 *
 * Registers native functions that wasm bytecode can call to submit
 * draw commands to the render queue (consumed by core 1).
 */
#include "wasm_export.h"
#include "esp_log.h"

#include "render_queue.h"
#include "tinygl_physics.h"

static const char *TAG = "render_api";

/* ── wasm-callable wrappers ─────────────────────────────────────────────── */

static void wasm_render_clear(wasm_exec_env_t env, int r, int g, int b)
{
    (void)env; (void)r; (void)g; (void)b;
}

static void wasm_render_submit_cube(wasm_exec_env_t env,
                                     float x, float y, float z,
                                     float size, float rx, float ry,
                                     int tex_id)
{
    (void)env;
    if (tex_id < 1 || tex_id > 4) tex_id = 1;
    render_queue_push((render_cmd_t){
        .x = x, .y = y, .z = z,
        .size = size,
        .rx = rx, .ry = ry,
        .tex_id = (unsigned int)tex_id,
    });
}

static void wasm_physics_step(wasm_exec_env_t env, float dt)
{
    (void)env;
    physics_step(dt);
}

static int wasm_physics_spawn(wasm_exec_env_t env,
                               float x, float y, float z, float size)
{
    (void)env;
    return physics_spawn(x, y, z, size);
}

static int wasm_physics_body_count(wasm_exec_env_t env)
{
    (void)env;
    int count = 0;
    for (int i = 0; i < MAX_BODIES; i++) {
        if (s_bodies[i].active) count++;
    }
    return count;
}

static void wasm_physics_body_pos(wasm_exec_env_t env,
                                   int idx,
                                   uint32_t x_ptr, uint32_t y_ptr, uint32_t z_ptr)
{
    body_t *b = physics_get(idx);
    if (!b || !b->active) return;

    wasm_module_inst_t inst = get_module_inst(env);

    if (x_ptr && wasm_runtime_validate_app_str_addr(inst, x_ptr))
        *(float *)wasm_runtime_addr_app_to_native(inst, x_ptr) = b->x;
    if (y_ptr && wasm_runtime_validate_app_str_addr(inst, y_ptr))
        *(float *)wasm_runtime_addr_app_to_native(inst, y_ptr) = b->y;
    if (z_ptr && wasm_runtime_validate_app_str_addr(inst, z_ptr))
        *(float *)wasm_runtime_addr_app_to_native(inst, z_ptr) = b->z;
}

/* ── registration ───────────────────────────────────────────────────────── */

#define REG_WASM(name, func, sig) do {                                  \
    NativeSymbol ns = {                                                 \
        .symbol = name,                                                 \
        .func_ptr = (void *)func,                                       \
        .signature = sig,                                               \
        .attachment = NULL,                                             \
    };                                                                  \
    if (!wasm_runtime_register_natives("env", &ns, 1)) {                \
        ESP_LOGE(TAG, "Failed to register %s", name);                   \
        return false;                                                   \
    }                                                                   \
} while(0)

bool render_api_register(void)
{
    REG_WASM("render_clear",          wasm_render_clear,          "(iii)");
    REG_WASM("render_submit_cube",    wasm_render_submit_cube,    "(fffffffiii)");
    REG_WASM("physics_step",          wasm_physics_step,          "(f)");
    REG_WASM("physics_spawn",         wasm_physics_spawn,         "(ffff)i");
    REG_WASM("physics_body_count",    wasm_physics_body_count,    "()i");
    REG_WASM("physics_body_pos",      wasm_physics_body_pos,      "(iiii)");

    ESP_LOGI(TAG, "Render + physics Host API registered (6 functions)");
    return true;
}